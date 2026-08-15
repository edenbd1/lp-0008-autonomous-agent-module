# `liblogosdelivery`, redistributed

`module/agent.lgx` carries a build of `liblogosdelivery` beside the plugin. This
directory is what that redistribution requires and the record of why it is a
redistribution at all rather than a download.

## What is shipped

| | |
|---|---|
| Upstream | [`logos-messaging/logos-delivery`](https://github.com/logos-messaging/logos-delivery) |
| Commit | `0d433ea83fb3bded3c2b116440a8b2b710f47991` |
| Built by | `make liblogosdelivery` (see `scripts/exercise-nodes.sh`, which builds it) |
| File in the package | `variants/darwin-arm64/liblogosdelivery.dylib`, 42,086,952 bytes |
| Licence | **MIT OR Apache-2.0**, at the recipient's election |
| Copyright | © 2025-2026 Logos |

`LICENSE-MIT` and `LICENSE-APACHE` here are upstream's own files, copied
unmodified, and `module/package-basecamp.sh` puts them into every package that
carries the library. MIT requires the copyright and permission notice to travel
"in all copies or substantial portions"; Apache-2.0 requires the licence, the
retained notices, and a statement of changes. There are no changes — the binary
is what `make liblogosdelivery` produced, not stripped and not re-signed — and
upstream ships no `NOTICE` file, so §4(d) does not apply.

The binary statically links `librln` v2.0.2 from `vendor/zerokit`
(vacp2p/zerokit, Apache-2.0 OR MIT, © 2022 Vac Research). Every other compiled
dependency in the checkout is MIT and/or Apache-2.0. The only GPL-family files
anywhere in upstream's tree are four Solidity test harnesses under
`vendor/waku-rlnv2-contract/lib/` (ds-test, GPL-3.0; erc4626-tests, AGPL-3.0);
they are Solidity, they are never compiled, and nothing from that submodule is
linked into the dylib.

## Why it is committed rather than fetched

Because there is nothing to fetch. This was checked rather than assumed, and the
checks are written down so nobody has to repeat them:

- **`logos-messaging/logos-delivery` publishes releases, and they have no
  assets.** Every release from `v0.37.0-beta` (2025-11) through `v0.38.1`
  (2026-05) carries zero files. The `nightly` pre-release carries
  `nwaku-arm64-macos-nightly.tar.gz`, which holds three executables —
  `wakunode2`, `logosdeliverynode`, `chat2` — and no library.
- **The one historical library asset is the wrong library.**
  `libwaku-v0.36.0-darwin-arm64.so` (2025-07) is real and pinnable
  (sha256 `9fb830cd8226572e463f0c1f4645056d76ae25ae6049de5cd6b5e7bc5b50ae51`),
  but it is `libwaku`: it exports `waku_*`, not the `logosdelivery_*` entry
  points this module resolves.
- **Upstream's release workflow builds a darwin-arm64 dylib and never publishes
  it.** `release-assets.yml` has a `macos-15 / arm64` leg that produces
  `liblogosdelivery-<version>-arm64-macos.tar.gz`, and its last step is
  `actions/upload-artifact` — the run artefact store, which is authenticated,
  expiring, and has no stable URL. There is no release-upload step in the file,
  so even a green run would publish nothing installable. The last green run was
  2025-10-16; every run since fails at `make: nimble: No such file or directory`.
- **Nowhere else publishes it in a pinnable form.** Not Nimble (source only, and
  no package entry), not Homebrew (no formula), not a container image (linux
  executables, 30-day expiry). The Logos nix cache genuinely holds an
  `aarch64-darwin` build — `/nix/store/gnpngvr44hwxnlv4z2pqa1srarcybjdc-liblogosdelivery-dev`,
  fetchable anonymously today — but its version is literally `dev` from a
  pull-request commit, the workflow that produces it runs only `on:
  pull_request`, attic caches garbage-collect, its signing key is not published
  in `flake.nix`, and consuming it requires nix on the install host. A pin that
  can evaporate is worse than a committed file, because it fails later and
  somewhere else.

`libstorage` is fetched rather than committed (`.github/workflows/ci.yml`,
`storage-node`) and that is not an inconsistency: its publisher,
`logos-storage/logos-storage-nim`, is a different organisation and does ship a
full asset matrix, darwin-arm64 included. The pattern is right; the delivery
repository simply does not offer it.

## What would let this be deleted

One upstream change: a release-upload step in `release-assets.yml`, and a green
run of it. On the day `liblogosdelivery-<version>-arm64-macos.tar.gz` appears on
a release page, this directory and the 16 MB in `module/agent.lgx` are replaced
by six lines of `curl` and a `sha256sum -c`, in the shape `storage-node` already
uses. The module needs no change to allow it: the library is opened by name with
`dlopen` at run time, not linked, so where it comes from is the installer's
business and not the plugin's.
