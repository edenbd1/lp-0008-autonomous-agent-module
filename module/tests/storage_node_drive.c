// Drive a real Logos Storage node through the lifecycle the agent skills use,
// and fail the process if any step does not actually happen.
//
// Same contract as delivery_node_drive.c: every step is an assertion and the
// exit code is the result. Upload is the interesting one — it is not one call
// but a session (init, then file), and the content address only exists once the
// node has really taken the bytes. Asserting on the CID is what separates "the
// call returned" from "the file is stored".
//
// Built and run by scripts/exercise-nodes.sh, which owns the paths.

#include "libstorage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int  done  = 0;
static volatile int  code  = 0;
static char          reply[4096];

static int failures = 0;
#define CHECK(cond, what)                                                      \
  do {                                                                         \
    if (cond) { printf("  ok    %s\n", (what)); }                              \
    else      { printf("  FAIL  %s\n", (what)); failures++; }                  \
  } while (0)

static void cb(int ret, const char *msg, size_t len, void *ud) {
  (void)ud;
  if (ret == RET_PROGRESS) return;             // not terminal
  if (msg && len) {
    size_t n = len < sizeof reply - 1 ? len : sizeof reply - 1;
    memcpy(reply, msg, n); reply[n] = 0;
  } else reply[0] = 0;
  code = ret; done = 1;
}

// Arm before the call, wait after it: the callback can fire before the call
// returns, so clearing the flag afterwards would lose the reply.
static void arm(void) { done = 0; code = -1; reply[0] = 0; }
static int  settled(int seconds) {
  for (int i = 0; i < seconds * 20; i++) { if (done) return code == RET_OK; usleep(50000); }
  return 0;
}

int main(void) {
  const char *cfg = "{\"log-level\":\"WARN\",\"data-dir\":\"./lp0008-storage\"}";

  printf("\n1. create a node\n");
  arm();
  void *ctx = storage_new(cfg, (StorageCallback)cb, NULL);
  CHECK(ctx != NULL, "storage_new returned a context");
  if (!ctx) { printf("\n1 failure(s)\n"); return 1; }
  CHECK(settled(30), "the node was constructed");

  printf("\n2. start it\n");
  arm();
  storage_start(ctx, (StorageCallback)cb, NULL);
  CHECK(settled(90), "the node started");

  printf("\n3. ask the running node who it is\n");
  arm();
  storage_peer_id(ctx, (StorageCallback)cb, NULL);
  int got_id = settled(30);
  printf("  <-    peer id: %s\n", reply);
  CHECK(got_id && reply[0], "it reported a peer id");

  printf("\n4. upload a file, as a session\n");
  const char *path = "lp0008-upload.txt";
  FILE *f = fopen(path, "w");
  if (!f) { printf("  FAIL  could not create %s\n", path); failures++; goto shutdown; }
  fputs("lp-0008 agent storage skill\n", f);
  fclose(f);

  // Every step below bails out on timeout instead of carrying on. Continuing
  // is not merely untidy: the callbacks carry no correlation, so a late reply
  // to a timed-out call satisfies the *next* wait. An upload_init that answers
  // slowly would hand its session id to the cid check, which would then pass
  // while nothing had been uploaded at all.
  arm();
  storage_upload_init(ctx, path, 65536, (StorageCallback)cb, NULL);
  if (!settled(60)) { printf("  FAIL  upload_init opened a session\n"); failures++; goto shutdown; }
  printf("  ok    upload_init opened a session\n");
  char session[512]; snprintf(session, sizeof session, "%s", reply);
  printf("  <-    session: %s\n", session);

  arm();
  storage_upload_file(ctx, session, (StorageCallback)cb, NULL);
  if (!settled(120)) { printf("  FAIL  the upload completed\n"); failures++; goto shutdown; }
  char cid[512]; snprintf(cid, sizeof cid, "%s", reply);
  printf("  <-    cid: %s\n", cid);
  // Not merely "some non-empty string": a Logos content address is base58 and
  // starts with 'z'. Asserting non-emptiness would accept the session id.
  CHECK(cid[0] == 'z' && strlen(cid) > 40, "the node returned a content address");

  printf("\n5. ask the node for it back\n");
  arm();
  storage_exists(ctx, cid, (StorageCallback)cb, NULL);
  int exists = settled(60);
  printf("  <-    exists: %s\n", reply);
  CHECK(exists && strstr(reply, "true"), "the content address is in the store");

  arm();
  storage_download_manifest(ctx, cid, (StorageCallback)cb, NULL);
  int manifest = settled(60);
  printf("  <-    manifest: %s\n", reply);
  // The manifest naming the file we uploaded is the part a stub cannot fake.
  CHECK(manifest && strstr(reply, "lp0008-upload.txt"),
        "its manifest names the file we uploaded");

  // Every early bail-out above lands here, so a node that was started always
  // gets stopped and destroyed. libstorage.h requires exactly that before
  // destroy, and leaking a running node would leave a repo directory locked
  // against the next run.
shutdown:
  printf("\n6. shut down\n");
  arm();
  storage_stop(ctx, (StorageCallback)cb, NULL);
  CHECK(settled(60), "the node stopped");
  arm();
  storage_close(ctx, (StorageCallback)cb, NULL);
  settled(30);
  CHECK(storage_destroy(ctx) == RET_OK, "the context was destroyed");

  unlink(path);
  printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "all steps confirmed", failures);
  return failures ? 1 : 0;
}
