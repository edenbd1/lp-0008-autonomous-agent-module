// The messaging half of the personal file vault, driven against a real Logos
// Delivery node.
//
//   share_drive <content-topic> <payload>
//
// `storage.share(address, recipient)` is a messaging act, not a storage one:
// content addressing means there is nothing to copy, so sharing a file is
// sending its address to someone. This is that send, over the owner channel
// topic, against the live Logos Delivery network.
//
// The payload goes out base64-encoded because that is what the node's message
// JSON takes. The encoding is done here rather than in the shell so that what is
// asserted — that the bytes the network carried are the content address the
// storage node returned — is checked against the same string in both halves.
//
// Every step is an assertion and the exit code is the result. In particular the
// send is not reported as successful the moment a `message_sent` event arrives:
// the documented failure flow is sent -> error, where the node gives up after
// its cache window and emits `message_error` a minute later. Breaking out on the
// first good news would make the error check unfalsifiable.

#include "liblogosdelivery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int created = -1;
static int started = -1;
static int stopped = -1;
static int sent_ok = 0;
static int send_err = 0;
static int received = 0;              // an onMessageReceived carrying our payload
static char peer_id[512] = {0};
static char last_err[512] = {0};
static char want_b64[4096] = {0};     // the payload we published, base64

static int failures = 0;
#define CHECK(cond, what)                                                      \
  do {                                                                         \
    if (cond) { printf("  ok    %s\n", (what)); }                              \
    else      { printf("  FAIL  %s\n", (what)); failures++; }                  \
  } while (0)

static void on_created(int ret, const char *ctxAddr, const char *errMsg, void *ud) {
  (void)ctxAddr; (void)ud;
  created = (ret == RET_OK);
  if (ret != RET_OK && errMsg) snprintf(last_err, sizeof last_err, "%s", errMsg);
}

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

static void on_scalar(int ret, char *msg, size_t len, void *ud) {
  const char *op = (const char *)ud;
  if (ret == NIMFFI_RET_STALE_WARN) return;              // progress tick
  int ok = (ret == RET_OK);
  printf("  <-    %s: %.*s\n", op, (int)len, msg ? msg : "");
  if (strcmp(op, "start_node") == 0) started = ok;
  if (strcmp(op, "stop_node") == 0) stopped = ok;
}

// The payload is NOT NUL-terminated — liblogosdelivery.h says so, and the bytes
// come from reused slot buffers, so whatever sits past `len` is the tail of an
// earlier, longer event. strstr() would read into that residue: a stale
// "message_error" turns a healthy send into a reported failure, and a stale
// "message_sent" is worse, because it is a false pass.
static int contains(const char *hay, size_t len, const char *needle) {
  size_t n = strlen(needle);
  if (n > len) return 0;
  for (size_t i = 0; i + n <= len; i++)
    if (memcmp(hay + i, needle, n) == 0) return 1;
  return 0;
}

static void on_event(int ret, const char *eventJson, size_t len, void *ud) {
  (void)ud;
  if (!eventJson || len == 0) return;
  printf("  event %.*s\n", (int)(len > 200 ? 200 : len), eventJson);
  if (ret != RET_OK) { send_err = 1; return; }
  // The name you register with is not the name that comes back: you subscribe
  // to "onMessageSent" and the payload carries "eventType":"message_sent".
  if (contains(eventJson, len, "\"message_sent\"") ||
      contains(eventJson, len, "\"message_propagated\"")) sent_ok = 1;
  if (contains(eventJson, len, "\"message_error\"")) send_err = 1;
  // A delivery back to a subscriber of the topic. Reported, not asserted: a
  // relay node is not required to hand a publisher its own message back, so
  // making the run depend on it would be asserting a property of the network
  // rather than of the agent.
  if (contains(eventJson, len, "\"message_received\"") &&
      want_b64[0] && contains(eventJson, len, want_b64)) received = 1;
}

static int wait_for(volatile int *flag, int seconds) {
  for (int i = 0; i < seconds * 10; i++) {
    if (*flag != -1 && *flag != 0) return 1;
    if (*flag == 0) return 0;                            // terminal failure
    usleep(100000);
  }
  return 0;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64(const unsigned char *in, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = in[i] << 16;
    if (i + 1 < n) v |= in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    if (o + 4 >= cap) break;
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
  }
  out[o] = 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <content-topic> <payload>\n", argv[0]);
    return 2;
  }
  const char *topic = argv[1];
  const char *payload = argv[2];

  b64((const unsigned char *)payload, strlen(payload), want_b64, sizeof want_b64);
  printf("  topic:   %s\n", topic);
  printf("  payload: %s\n", payload);
  printf("  base64:  %s\n", want_b64);

  printf("\n1. the agent's messaging node\n");
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
  CHECK(wait_for(&created, 20), "the node was constructed");
  if (created != 1) {
    printf("  error: %s\n", last_err[0] ? last_err : "unknown");
    logosdelivery_destroy(ctx);
    printf("\n%d failure(s)\n", failures);
    return 1;
  }

  printf("\n2. listen before anything is sent\n");
  // add_event_listener returns a listener id, and 0 means it did not register.
  // Printing "ok" without looking turns a failed registration into a confusing
  // timeout below, and into a line that reads as a step that passed.
  uint64_t l1 = logosdelivery_add_event_listener(ctx, "onMessageSent", on_event, NULL);
  uint64_t l2 = logosdelivery_add_event_listener(ctx, "onMessagePropagated", on_event, NULL);
  uint64_t l3 = logosdelivery_add_event_listener(ctx, "onMessageError", on_event, NULL);
  uint64_t l4 = logosdelivery_add_event_listener(ctx, "onMessageReceived", on_event, NULL);
  CHECK(l1 && l2 && l3 && l4, "four listeners registered");

  printf("\n3. start it\n");
  logosdelivery_start_node(ctx, on_scalar, (void *)"start_node");
  CHECK(wait_for(&started, 90), "the node started");

  printf("\n4. who is it\n");
  LogosdeliveryGetNodeInfoReq who = { .nodeInfoId = "MyPeerId" };
  logosdelivery_get_node_info(ctx, on_reply, (void *)"MyPeerId", &who);
  sleep(2);
  CHECK(peer_id[0] != 0, "it reported a peer id");

  printf("\n5. subscribe to the owner channel, then share the address on it\n");
  LogosdeliverySubscribeReq sub = { .contentTopicStr = topic };
  logosdelivery_subscribe(ctx, on_reply, (void *)"subscribe", &sub);
  sleep(3);

  char msg[8192];
  snprintf(msg, sizeof msg,
           "{\"contentTopic\": \"%s\", \"payload\": \"%s\", \"ephemeral\": false}",
           topic, want_b64);
  LogosdeliverySendReq send = { .messageJson = msg };
  logosdelivery_send(ctx, on_reply, (void *)"send", &send);

  // Wait out the cache window even after success, and let a later error win.
  for (int i = 0; i < 900 && !send_err; i++) usleep(100000);
  CHECK(sent_ok && !send_err, "the address reached the network, and stayed sent");
  printf("  note  delivered back to this subscriber: %s\n", received ? "yes" : "not observed");

  printf("\n6. shut down\n");
  logosdelivery_stop_node(ctx, on_scalar, (void *)"stop_node");
  wait_for(&stopped, 30);
  CHECK(stopped == 1, "the node stopped");
  CHECK(logosdelivery_destroy(ctx) == RET_OK, "the context was destroyed");

  printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "all steps confirmed", failures);
  return failures ? 1 : 0;
}
