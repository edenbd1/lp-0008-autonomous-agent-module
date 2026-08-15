#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Build `agent-console` and a third-party skill, load the skill into the agent
# module, and call it — then check the answer against something outside this
# repository.
#
# This script is the evidence for the prize's usability criterion, "a documented
# skill interface (module/SDK) that can be used to add new skills without
# modifying the core agent module". It is written so that a green exit means
# something a transcript cannot fake:
#
#   1. The skill is compiled with `-I module/src` and nothing else — no Logos
#      SDK header, no Qt, no JSON library — and it is not linked against the
#      module. If it ever needed more than `agent_module_interface.h`, this line
#      stops compiling. `-Werror` so a warning cannot be scrolled past.
#   2. `git status --porcelain module/` must be unchanged by the run. Editing any
#      file the agent module ships — a source, a header, `CMakeLists.txt` or the
#      packaged `agent.lgx` — fails the script. That is the criterion's own
#      words, checked rather than asserted.
#   3. The digest the skill returns is compared against `shasum -a 256` of the
#      same input. A skill that was registered but never ran cannot pass this.
#   4. A control: the same digest must NOT match the digest of altered input.
#      Without it, step 3 would also pass if both sides computed nothing.
#
# Usage:
#   examples/agent-console/run.sh [content-to-notarise]
#
# Environment:
#   LOGOS_CPP_SDK_ROOT  a checkout of github.com/logos-co/logos-cpp-sdk. Only two
#                       headers are reached and both are Qt-free:
#                       logos_module_context.h and logos_result.h.
#                       Default: $HOME/logos/src/logos-cpp-sdk
#   NLOHMANN_INCLUDE    directory holding nlohmann/json.hpp, if it is not already
#                       on the include path. Needed by the *module*, not by the
#                       skill. Default: /opt/homebrew/include on macOS.
#   OUT                 where the binaries go. Default: $TMPDIR/lp0008-console
#   CXX                 default: c++

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
OUT="${OUT:-${TMPDIR:-/tmp}/lp0008-console}"
mkdir -p "$OUT"

CXX="${CXX:-c++}"
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"
case "$(uname -s)" in
    Darwin) NLOHMANN_INCLUDE="${NLOHMANN_INCLUDE:-/opt/homebrew/include}"; SO=dylib ;;
    *)      NLOHMANN_INCLUDE="${NLOHMANN_INCLUDE:-/usr/include}";          SO=so    ;;
esac

if [ ! -f "$SDK/cpp/logos_module_context.h" ]; then
    echo "logos-cpp-sdk not found at $SDK" >&2
    echo "set LOGOS_CPP_SDK_ROOT to a checkout of github.com/logos-co/logos-cpp-sdk" >&2
    echo "(README §9 pins the revision CI uses)" >&2
    exit 1
fi
if [ ! -f "$NLOHMANN_INCLUDE/nlohmann/json.hpp" ] && [ ! -f /usr/include/nlohmann/json.hpp ]; then
    echo "nlohmann/json.hpp not found; set NLOHMANN_INCLUDE" >&2
    exit 1
fi

echo "[0/5] the tree before the build"
before="$(cd "$repo" && git status --porcelain module/ 2>/dev/null || true)"
if [ -n "$before" ]; then
    echo "  --    module/ is already dirty before this script runs:"
    printf '%s\n' "$before" | sed 's/^/        /'
    echo "  --    step 5 compares before against after, so it still means something,"
    echo "  --    but a clean tree is the stronger check"
else
    echo "  ok    module/ is clean"
fi

# ---------------------------------------------------------------------------
# 1. The skill. One include path, and it is the published interface header.
#
# Deliberately NOT -I$SDK/cpp, NOT -I$NLOHMANN_INCLUDE, and no module object on
# the link line. `-undefined dynamic_lookup` is not needed either: `ISkill` is
# pure virtual with an inline destructor, so it is entirely header-supplied and
# the library has no unresolved symbol from the module.
# ---------------------------------------------------------------------------
echo "[1/5] compile the skill against the interface header alone"
"$CXX" -std=c++17 -Wall -Wextra -Werror -fPIC -shared -O2 \
    -I"$repo/module/src" \
    "$repo/examples/skills/notary-digest/notary_digest_skill.cpp" \
    -o "$OUT/libnotary_digest.$SO"
echo "  ok    libnotary_digest.$SO built from one header: agent_module_interface.h"

# ---------------------------------------------------------------------------
# 2. The console, linking the agent module exactly as it is shipped.
#
# The source list is the one in module/CMakeLists.txt minus task_persistence.cpp,
# which the console does not construct — same as the plugin.
# ---------------------------------------------------------------------------
echo "[2/5] compile agent-console, linking the agent module unmodified"
"$CXX" -std=c++17 -Wall -Wextra -O1 \
    -I"$repo/module/src" -I"$SDK/cpp" -I"$NLOHMANN_INCLUDE" \
    "$here/console.cpp" \
    "$repo/module/src/agent_module_plugin.cpp" \
    "$repo/module/src/messaging_skills.cpp" \
    "$repo/module/src/storage_skills.cpp" \
    "$repo/module/src/inference.cpp" \
    "$repo/module/src/wallet_skills.cpp" \
    "$repo/module/src/program_skills.cpp" \
    "$repo/module/src/agent_skills.cpp" \
    "$repo/module/src/owner_channel.cpp" \
    -o "$OUT/agent-console"
echo "  ok    $OUT/agent-console"

# ---------------------------------------------------------------------------
# 3. The self-test. `--offline` so this step needs no network and cannot pass or
#    fail on someone else's uptime — the chain reads are exercised in step 4.
# ---------------------------------------------------------------------------
CONTENT="${1:-LP-0008: a skill the agent module was never built with.}"
echo "[3/5] load the skill into the module and call it"
"$OUT/agent-console" --offline --skill "$OUT/libnotary_digest.$SO" \
    selftest "$CONTENT" | tee "$OUT/selftest.txt"

# ---------------------------------------------------------------------------
# 4. The console's other half: the module answering from a shell.
# ---------------------------------------------------------------------------
echo "[4/5] the same module, answering as a command-line tool"
echo "  ->    agent-console --offline --skill … skills   (names only)"
"$OUT/agent-console" --offline --skill "$OUT/libnotary_digest.$SO" skills \
  | tr ',' '\n' | grep '"name"' | sed 's/.*"name":"/        /; s/"//' | tr '\n' ' '
echo
count="$("$OUT/agent-console" --offline --skill "$OUT/libnotary_digest.$SO" skills \
         | grep -o '"name"' | wc -l | tr -d ' ')"
echo "  ok    $count skills registered (22 built in, plus each --skill library)"

# The reference table in docs/skills.md is a snapshot of a thing that moves, and
# a snapshot nobody checks is how a document comes to describe a skill that was
# renamed two commits ago. Compare the registered names against the ones the
# document spells in `code`, and fail on any difference in either direction.
"$OUT/agent-console" --offline skills \
  | tr ',' '\n' | sed -n 's/.*"name":"\([^"]*\)".*/\1/p' | sort -u > "$OUT/names.module"
sed -n '/^## 7\. Reference/,/^## Status, honestly/p' "$repo/docs/skills.md" \
  | grep -o '^| `[a-z_]*\.[a-z_]*`' | tr -d '|` ' | sort -u > "$OUT/names.doc"
if ! diff -u "$OUT/names.doc" "$OUT/names.module" > "$OUT/names.diff"; then
    echo "  FAIL  docs/skills.md §7 no longer lists what the module registers:"
    sed 's/^/        /' "$OUT/names.diff"
    exit 1
fi
echo "  ok    docs/skills.md §7 lists exactly the $(wc -l < "$OUT/names.module" | tr -d ' ') skills the module registers"

# ---------------------------------------------------------------------------
# 5. The three checks that make the transcript mean something.
# ---------------------------------------------------------------------------
echo "[5/5] check the answer against something that is not this repository"

returned="$(awk '/^DIGEST /{print $2}' "$OUT/selftest.txt")"
if [ -z "$returned" ]; then
    echo "  FAIL  the self-test printed no digest"
    exit 1
fi
expected="$(printf '%s' "$CONTENT" | shasum -a 256 | awk '{print $1}')"
if [ "$returned" != "$expected" ]; then
    echo "  FAIL  the skill returned $returned; shasum -a 256 says $expected"
    exit 1
fi
echo "  ok    the digest matches shasum -a 256 of the same input"

control="$(printf '%s' "$CONTENT tampered" | shasum -a 256 | awk '{print $1}')"
if [ "$returned" = "$control" ]; then
    echo "  FAIL  it matches altered input too, so the comparison proves nothing"
    exit 1
fi
echo "  ok    and does not match the digest of altered input"

after="$(cd "$repo" && git status --porcelain module/ 2>/dev/null || true)"
if [ "$before" != "$after" ]; then
    echo "  FAIL  the run modified the agent module:"
    diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") || true
    exit 1
fi
echo "  ok    nothing under module/ was modified — the criterion's words, checked"

echo
echo "a third party added a skill to this module without editing it"
