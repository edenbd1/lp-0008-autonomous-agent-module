// Drive a real Logos Delivery node through the lifecycle the agent skills use,
// and fail the process if any step does not actually happen.
//
// The shipped example prints what each call returned and exits 0 either way.
// That is fine for a tutorial and useless as evidence: a node that never
// started would produce a transcript that reads like success. Every step here
// is an assertion, and the exit code is the result.
//
// Built and run by scripts/exercise-nodes.sh, which owns the paths.

#include "liblogosdelivery.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int  created   = -1;   // create_node terminal result
static int  started   = -1;   // start_node terminal result
static int  stopped   = -1;
static int  sent_ok   =  0;   // an onMessageSent / onMessagePropagated arrived
static int  send_err  =  0;
static char peer_id[512] = {0};
static char last_err[512] = {0};

static int failures = 0;
#define CHECK(cond, what)                                                      \
  do {                                                                         \
    if (cond) { printf("  ok    %s\n", (what)); }                              \
    else      { printf("  FAIL  %s\n", (what)); failures++; }                  \
  } while (0)

// Terminal result of create_node.
static void on_created(int ret, const char *ctxAddr, const char *errMsg, void *ud) {
  (void)ctxAddr; (void)ud;
  created = (ret == RET_OK);
  if (ret != RET_OK && errMsg) snprintf(last_err, sizeof last_err, "%s", errMsg);
}

// Replies for the argument-taking calls. userData carries which call it was.
static void on_reply(int ret, const char *reply, const char *errMsg, void *ud) {
  const char *op = (const char *)ud;
  if (ret == RET_OK) {
    printf("  <-    %s: %s\n", op, reply ? reply : "");
    if (reply && strcmp(op, "MyPeerId") == 0)
      snprintf(peer_id, sizeof peer_id, "%s", reply);
  } else {
    printf("  <-    %s FAILED: %s\n", op, errMsg ? errMsg : "unknown");
  }
}

// Raw scalar replies (start_node / stop_node). msg is len bytes, not NUL-terminated.
static void on_scalar(int ret, char *msg, size_t len, void *ud) {
  const char *op = (const char *)ud;
  if (ret == NIMFFI_RET_STALE_WARN) return;            // progress tick, not terminal
  int ok = (ret == RET_OK);
  printf("  <-    %s: %.*s\n", op, (int)len, msg ? msg : "");
  if (strcmp(op, "start_node") == 0) started = ok;
  if (strcmp(op, "stop_node")  == 0) stopped = ok;
}

static void on_event(int ret, const char *eventJson, size_t len, void *ud) {
  (void)ret; (void)ud;
  if (!eventJson) return;
  printf("  event %.*s\n", (int)(len > 160 ? 160 : len), eventJson);
  // The name you register with is not the name that comes back: you subscribe
  // to "onMessageSent" and the payload carries "eventType":"message_sent".
  // Matching the registration name silently never fires, which reads exactly
  // like a message that never left.
  if (strstr(eventJson, "\"message_sent\"") ||
      strstr(eventJson, "\"message_propagated\"")) sent_ok = 1;
  if (strstr(eventJson, "\"message_error\""))      send_err = 1;
}

// Poll a flag rather than sleeping a fixed amount: a fixed sleep either wastes
// time or races, and a race here would report a healthy node as broken.
static int wait_for(volatile int *flag, int seconds) {
  for (int i = 0; i < seconds * 10; i++) {
    if (*flag != -1 && *flag != 0) return 1;
    if (*flag == 0) return 0;                   // terminal failure
    usleep(100000);
  }
  return 0;
}

int main(void) {
  const char *topic = "/lp0008/1/agent/proto";

  printf("\n1. create a node\n");
  // Core mode on the dev preset: the same pair the shipped example uses, so a
  // failure here is ours and not a guess at an unsupported network.
  const char *config =
    "{"
      "\"mode\": \"Core\","
      "\"preset\": \"logos.dev\","
      "\"messagingOverrides\": { \"log-level\": \"WARN\" }"
    "}";
  LogosdeliveryCreateNodeCtorReq req = { .configJson = config };
  void *ctx = logosdelivery_create_node(&req, on_created, NULL);
  CHECK(ctx != NULL, "create_node returned a context");
  if (!ctx) { printf("\n%d failure(s)\n", ++failures); return 1; }
  CHECK(wait_for(&created, 15), "the node was constructed");
  if (created != 1) {
    printf("  error: %s\n", last_err[0] ? last_err : "unknown");
    logosdelivery_destroy(ctx);
    printf("\n%d failure(s)\n", failures);
    return 1;
  }

  printf("\n2. listen for message events before anything is sent\n");
  logosdelivery_add_event_listener(ctx, "onMessageSent",       on_event, NULL);
  logosdelivery_add_event_listener(ctx, "onMessagePropagated", on_event, NULL);
  logosdelivery_add_event_listener(ctx, "onMessageError",      on_event, NULL);
  printf("  ok    three listeners registered\n");

  printf("\n3. start it, and wait for the node to say so\n");
  // start_node returns once dispatched; the scalar callback is the real answer.
  logosdelivery_start_node(ctx, on_scalar, (void *)"start_node");
  CHECK(wait_for(&started, 60), "the node started");

  printf("\n4. ask the running node who it is\n");
  LogosdeliveryGetNodeInfoReq who = { .nodeInfoId = "MyPeerId" };
  logosdelivery_get_node_info(ctx, on_reply, (void *)"MyPeerId", &who);
  sleep(2);
  CHECK(peer_id[0] != 0, "it reported a peer id");

  printf("\n5. subscribe, then publish to the same topic\n");
  LogosdeliverySubscribeReq sub = { .contentTopicStr = topic };
  logosdelivery_subscribe(ctx, on_reply, (void *)"subscribe", &sub);
  sleep(2);

  // "lp-0008 agent" in base64.
  const char *msg =
    "{"
      "\"contentTopic\": \"/lp0008/1/agent/proto\","
      "\"payload\": \"bHAtMDAwOCBhZ2VudA==\","
      "\"ephemeral\": false"
    "}";
  LogosdeliverySendReq send = { .messageJson = msg };
  logosdelivery_send(ctx, on_reply, (void *)"send", &send);

  for (int i = 0; i < 600 && !sent_ok && !send_err; i++) usleep(100000);
  CHECK(sent_ok && !send_err, "the message reached the network");

  printf("\n6. shut down\n");
  logosdelivery_stop_node(ctx, on_scalar, (void *)"stop_node");
  wait_for(&stopped, 30);
  CHECK(stopped == 1, "the node stopped");
  CHECK(logosdelivery_destroy(ctx) == RET_OK, "the context was destroyed");

  printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "all steps confirmed", failures);
  return failures ? 1 : 0;
}
