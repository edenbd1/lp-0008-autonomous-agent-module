// The storage half of the personal file vault, driven against a real Logos
// Storage node.
//
//   vault_drive <data-dir> <upload-path> <download-path>
//
// The agent takes a file, hands it to the node, and gets back a content
// address. Then it asks for that address back and writes the bytes to a second
// path. The shell around it does the encryption and the comparison, because
// those are the parts a reader can redo with `openssl` and `shasum` — this
// binary's only job is to be the node, and to fail the process if any step did
// not actually happen.
//
// Structurally the same contract as module/tests/storage_node_drive.c: every
// step is an assertion, the exit code is the result, and a node that silently
// failed to start cannot produce a passing transcript. What is new here is the
// retrieval — the existing driver stops at the manifest, and a manifest is not
// the file. Nothing about "the owner can get it back" is demonstrated until the
// bytes come back out.
//
// The control is a content address that cannot exist: the real CID with one
// character changed. A Logos content address carries no checksum over that
// field, so the mutant is well formed and addresses nothing.
//
// WHAT THE CONTROL IS ASKED, AND WHY IT IS NOT `exists`
//
// `storage_exists` answers TRUE for this mutant. Measured, not assumed: for a
// real address ending `…2iyAWQbx`, the node also answers `true` for
// `…2iyAWqbx`, while other one-character changes come back `false`. It is a
// datastore key lookup — `hasLocalBlock` is `cid in localStore` — and a near
// miss can land on a key that is present, so it reports that something is
// stored under that key and not that the address resolves.
//
// `storage_download_manifest` on the same mutant fails, and says exactly why:
//
//   Failed to fetch manifest: Cid doesn't match the data
//
// which is the store finding a block and the CID not being the address of it.
// So the control asks for the data, and `exists` is printed for both addresses
// and believed for neither. An `exists`-only control would have passed this
// driver while proving nothing — it was written that way first, and this is
// what the run said.
//
// The control runs LAST, after the real retrieval has been asserted. A failing
// manifest fetch can time out rather than answer, and a late reply to a
// timed-out call satisfies the next wait: putting the control first would let
// its stale reply arrive in the middle of a real assertion.

#include "libstorage.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int done = 0;
static volatile int code = 0;
static char reply[8192];

static int failures = 0;
#define CHECK(cond, what)                                                      \
  do {                                                                         \
    if (cond) { printf("  ok    %s\n", (what)); }                              \
    else      { printf("  FAIL  %s\n", (what)); failures++; }                  \
  } while (0)

static void cb(int ret, const char *msg, size_t len, void *ud) {
  (void)ud;
  if (ret == RET_PROGRESS) return;               // a chunk, not the answer
  if (msg && len) {
    size_t n = len < sizeof reply - 1 ? len : sizeof reply - 1;
    memcpy(reply, msg, n); reply[n] = 0;
  } else reply[0] = 0;
  code = ret; done = 1;
}

// Arm before the call, wait after it: the callback can fire before the call
// returns, so clearing the flag afterwards would lose the reply.
static void arm(void) { done = 0; code = -1; reply[0] = 0; }
static int settled(int seconds) {
  for (int i = 0; i < seconds * 20; i++) { if (done) return code == RET_OK; usleep(50000); }
  return 0;
}

// A content address that cannot exist, derived from one that does. Changing a
// character in the digest keeps the string a syntactically valid base58 CID and
// makes it name nothing. The first characters are the CID prefix (codec,
// version, multihash) and are left alone, so what is being tested is the
// address of the content and not the shape of the string.
static void mutate(const char *cid, char *out, size_t n) {
  snprintf(out, n, "%s", cid);
  size_t len = strlen(out);
  if (len < 12) return;
  size_t at = len - 3;
  out[at] = (out[at] == 'q') ? 'r' : 'q';   // both are in the base58 alphabet
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <data-dir> <upload-path> <download-path>\n", argv[0]);
    return 2;
  }
  const char *data_dir = argv[1];
  const char *upload_path = argv[2];
  const char *download_path = argv[3];

  // The file the manifest has to name is the basename, not the path the shell
  // handed in. Checking for the full path would fail on a node that is behaving
  // correctly, which is the worst kind of check.
  const char *base = strrchr(upload_path, '/');
  base = base ? base + 1 : upload_path;

  char cfg[1024];
  snprintf(cfg, sizeof cfg, "{\"log-level\":\"WARN\",\"data-dir\":\"%s\"}", data_dir);

  printf("\n1. the agent's storage node\n");
  arm();
  void *ctx = storage_new(cfg, (StorageCallback)cb, NULL);
  CHECK(ctx != NULL, "storage_new returned a context");
  if (!ctx) { printf("\n1 failure(s)\n"); return 1; }
  CHECK(settled(30), "the node was constructed");

  arm();
  storage_start(ctx, (StorageCallback)cb, NULL);
  CHECK(settled(120), "the node started");

  arm();
  storage_peer_id(ctx, (StorageCallback)cb, NULL);
  int got_id = settled(30);
  printf("  <-    peer id: %s\n", reply);
  CHECK(got_id && reply[0], "it reported a peer id");

  char cid[512] = {0};

  printf("\n2. the agent stores the encrypted file\n");
  // Bail out on timeout rather than carry on: the callbacks carry no
  // correlation, so a late reply to a timed-out call satisfies the *next* wait.
  // A slow upload_init would hand its session id to the CID check.
  arm();
  storage_upload_init(ctx, upload_path, 65536, (StorageCallback)cb, NULL);
  if (!settled(60)) { printf("  FAIL  upload_init opened a session\n"); failures++; goto shutdown; }
  printf("  ok    upload_init opened a session\n");
  char session[512]; snprintf(session, sizeof session, "%s", reply);

  arm();
  storage_upload_file(ctx, session, (StorageCallback)cb, NULL);
  if (!settled(180)) { printf("  FAIL  the upload completed\n"); failures++; goto shutdown; }
  snprintf(cid, sizeof cid, "%s", reply);
  printf("  <-    cid: %s\n", cid);
  // Not "some non-empty string": a Logos content address is base58 and starts
  // with 'z'. Asserting non-emptiness would accept the session id.
  CHECK(cid[0] == 'z' && strlen(cid) > 40, "the node returned a content address");
  if (!(cid[0] == 'z' && strlen(cid) > 40)) goto shutdown;

  arm();
  storage_download_manifest(ctx, cid, (StorageCallback)cb, NULL);
  int manifest = settled(60);
  printf("  <-    manifest: %s\n", reply);
  CHECK(manifest && strstr(reply, base), "its manifest names the file that was stored");

  printf("\n3. the owner asks for the address back\n");
  arm();
  storage_exists(ctx, cid, (StorageCallback)cb, NULL);
  int exists = settled(60);
  printf("  <-    exists: %s\n", reply);
  CHECK(exists && strstr(reply, "true"), "the real address is in the store");

  arm();
  storage_download_init(ctx, cid, 65536, true, (StorageCallback)cb, NULL);
  if (!settled(60)) { printf("  FAIL  download_init opened a session\n"); failures++; goto shutdown; }
  printf("  ok    download_init opened a session\n");

  arm();
  storage_download_stream(ctx, cid, 65536, true, download_path, (StorageCallback)cb, NULL);
  int downloaded = settled(180);
  CHECK(downloaded, "the node streamed the content back to a file");

  printf("\n4. control: an address that cannot exist\n");
  char ghost[512];
  mutate(cid, ghost, sizeof ghost);
  printf("  <-    ghost: %s\n", ghost);
  CHECK(strcmp(ghost, cid) != 0, "the control address differs from the real one");
  // Printed, deliberately not asserted on: see the note at the top. This line
  // is here so a reader can see the flag say the wrong thing.
  arm();
  storage_exists(ctx, ghost, (StorageCallback)cb, NULL);
  int ghost_exists = settled(60);
  printf("  <-    exists: %s   (a key lookup, not an answer about the address)\n",
         ghost_exists ? reply : "(error)");
  // This is the control. The node must not be able to produce the manifest for
  // an address it does not hold — and for the real one, in step 2, it could.
  arm();
  storage_download_manifest(ctx, ghost, (StorageCallback)cb, NULL);
  int ghost_manifest = settled(90);
  printf("  <-    manifest: %.120s\n", ghost_manifest ? reply : reply[0] ? reply : "(no answer)");
  CHECK(!(ghost_manifest && strstr(reply, "treeCid")),
        "the node cannot produce the content behind the control address");

  // Every early bail-out above lands here, so a node that was started always
  // gets stopped and destroyed. libstorage.h requires exactly that before
  // destroy, and leaking a running node leaves the repo directory locked
  // against the next run.
shutdown:
  printf("\n5. shut down\n");
  arm();
  storage_stop(ctx, (StorageCallback)cb, NULL);
  CHECK(settled(60), "the node stopped");
  arm();
  storage_close(ctx, (StorageCallback)cb, NULL);
  settled(30);
  CHECK(storage_destroy(ctx) == RET_OK, "the context was destroyed");

  // Last line, machine-readable, so the shell around this does not have to
  // scrape a human-facing transcript for the address.
  if (cid[0]) printf("\nCID %s\n", cid);
  printf("%s (%d failure(s))\n", failures ? "FAILED" : "all steps confirmed", failures);
  return failures ? 1 : 0;
}
