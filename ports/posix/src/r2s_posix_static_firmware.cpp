// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* Static posix firmware: the same Float32, Twist, String, Time and JointState
 * topics as the generated C firmware, announced from a fixed R2S_TOPICS list
 * through the static C++ API, so the end-to-end suite drives the static path
 * as a process on a pty. The command line mirrors the C firmware's, and the
 * topics announce in the same order with IDs from 16. Renaming or dropping
 * a row is rejected: the static list fixes the announce, so only
 * --no-log-text silences its traffic while staying announced. */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define R2S_TOPICS(X) \
  X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
  X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel) \
  X(Publisher, geometry_msgs::msg::Twist, echo_vel_pub, "echo_vel") \
  X(Publisher, std_msgs::msg::String, log_text_pub, "log_text") \
  X(Publisher, builtin_interfaces::msg::Time, time_now_pub, "time_now") \
  X(Subscription, sensor_msgs::msg::JointState, joints_sub, "joints", on_joints)
#include "ros2serial.h"

#include <builtin_interfaces/msg/time.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/string.hpp>

#include <geometry_msgs/msg/twist.hpp>

#include "r2s/r2s.h"
#include "r2s/r2s_session.h"

#include "r2s_posix_port.h"

/* Publish rates in Hz, from once a second to the loop's 1 ms granularity. */
#define RATE_MIN 1u
#define RATE_MAX 100u

/* Default topic names the command-line flags rename. */
static const char *const kDefaultPub = "battery";
static const char *const kDefaultSub = "cmd_vel";
static const char *const kDefaultEcho = "echo_vel";
static const char *const kDefaultLogText = "log_text";
static const char *const kDefaultTimeNow = "time_now";

/* The skew range the wire correction carries (spec/protocol.md [TIM-4]):
 * past it the host must refuse the correction, so the knob stops here. */
#define SKEW_MIN_PPM (-20000)
#define SKEW_MAX_PPM 20000

struct options
{
  const char *port;
  const char *node;
  const char *pub;
  const char *sub;
  const char *echo;
  const char *log_text;
  const char *time_now;
  int32_t skew_ppm;
  unsigned rate;
  uint8_t log_lines;
};

/* The echo the callback publishes on: the static object always exists, so
 * a frame that arrives before the table closes is refused by the session
 * instead of reaching the callback with nowhere to go. */
static uint32_t s_cmd_vel_count;

void on_cmd_vel(const geometry_msgs::msg::Twist & msg)
{
  s_cmd_vel_count++;
  fprintf(stderr, "r2s_posix: cmd_vel n=%lu\n", (unsigned long)s_cmd_vel_count);
    /* Best effort echo for scenario 3: five messages on an idle link never
     * fill the port, so the return carries no signal the test would use. */
  (void)echo_vel_pub.publish(msg);
}

static uint32_t s_joints_count;

void on_joints(const sensor_msgs::msg::JointState & msg)
{
  (void)msg;
  s_joints_count++;
  fprintf(stderr, "r2s_posix: joints n=%lu\n", (unsigned long)s_joints_count);
}

static void usage(FILE *out)
{
  fprintf(out, "Usage: r2s_posix_static_firmware PORT [--node NAME] [--rate HZ]\n"
                 "           [--pub NAME | --no-pub] [--sub NAME | --no-sub]\n"
                 "           [--echo NAME | --no-echo] [--time-now NAME | --no-time-now]\n"
                 "           [--joints NAME | --no-joints]\n"
                 "           [--log-text NAME | --no-log-text] [--log-lines] [--tick-skew-ppm PPM]\n");
}

static const char * take_value(int argc, char **argv, int *i, const char *flag)
{
  if (*i + 1 >= argc) {
    fprintf(stderr, "r2s_posix: %s needs a value\n", flag);
    usage(stderr);
    exit(2);
  }
  (*i)++;
  return argv[*i];
}

static unsigned take_rate(const char *text)
{
  char *end;
  long rate;

  errno = 0;
  rate = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || rate < (long)RATE_MIN ||
    rate > (long)RATE_MAX)
  {
    fprintf(stderr, "r2s_posix: --rate needs %u..%u, got '%s'\n", RATE_MIN, RATE_MAX, text);
    usage(stderr);
    exit(2);
  }
  return (unsigned)rate;
}

static int32_t take_skew(const char *text)
{
  char *end;
  long skew;

  errno = 0;
  skew = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || skew < (long)SKEW_MIN_PPM ||
    skew > (long)SKEW_MAX_PPM)
  {
    fprintf(stderr, "r2s_posix: --tick-skew-ppm needs %d..%d, got '%s'\n", SKEW_MIN_PPM,
      SKEW_MAX_PPM,
                text);
    usage(stderr);
    exit(2);
  }
  return (int32_t)skew;
}

static void check_name(const char *flag, const char *name)
{
  if (name[0] == '\0') {
    fprintf(stderr, "r2s_posix: %s needs a non-empty name\n", flag);
    usage(stderr);
    exit(2);
  }
}

/* Scratch name the node probe below configures: overlong candidates are
 * refused, and the static Node's first spin binds the launch name before
 * use. Static storage, so the pointer the core keeps stays valid until then. */
static char s_node_probe[R2S_MAX_FRAME];

/* Whether the core gate binds a node name of len x's with no topics: the
 * probe asks r2s_session_configure() itself instead of copying its sum. */
static uint8_t node_probe_fits(uint32_t len)
{
  uint32_t i;

  if (len >= (uint32_t)R2S_MAX_FRAME) {
    return 0u;
  }
  for (i = 0u; i < len; i++) {
    s_node_probe[i] = 'x';
  }
  s_node_probe[len] = '\0';
  return (uint8_t)(r2s_session_configure(s_node_probe, NULL, 0u) == R2S_OK);
}

/* Longest node name the core gate still binds: a binary search finds the
 * exact edge, so the configured name stays the length bound with no hidden
 * headroom. */
static size_t node_name_limit(void)
{
  uint32_t lo = 0u;
  uint32_t hi = (uint32_t)R2S_MAX_FRAME;

  if (node_probe_fits(0u) == 0u) {
    return 0u;
  }
  while (lo + 1u < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;

    if (node_probe_fits(mid) != 0u) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return (size_t)lo;
}

/* A node name that would not fit the hello entry is rejected here, with its
 * length and the limit, instead of leaving the node unconnected with only
 * a handshake timeout to show for it. */
static void check_node_name(const char *flag, const char *name)
{
  size_t len;
  size_t limit;

  check_name(flag, name);
  len = strlen(name);
  limit = node_name_limit();
  if (len > limit) {
    fprintf(stderr, "r2s_posix: %s node name '%s' too long: %lu chars, limit %lu\n", flag, name,
      (unsigned long)len, (unsigned long)limit);
    usage(stderr);
    exit(2);
  }
}

/* A rename or a drop the static list cannot express: the announce carries
 * the compiled rows, so the flag fails here instead of silently keeping
 * the old name on the wire. */
static void reject_list_change(const char *flag, const char *name)
{
  fprintf(stderr, "r2s_posix: %s names '%s', but the static list fixes every topic\n", flag, name);
  usage(stderr);
  exit(2);
}

/* One log_text line: the value as 8 hex digits. The bundled String bound
 * holds 16 characters, so the 8 digits fit with room to spare. */
static bool publish_log_text(uint32_t value)
{
  std_msgs::msg::String msg;
  uint8_t i;

  memset(&msg, 0, sizeof msg);
  for (i = 0u; i < 8u; i++) {
    int digit = (int)((value >> 28u) & 0xFu);

    msg.data[i] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    value <<= 4u;
  }
  msg.data[8] = '\0';
  return log_text_pub.publish(msg) == R2S_OK;
}

/* One line per level every LOG_PERIOD_MS while connected, so the host log
 * test sees every severity. Plain text, which the bridge renders verbatim. */
#define LOG_PERIOD_MS 2000u

static void emit_log_lines(void)
{
  R2S_LOG_DEBUG("r2s log debug");
  R2S_LOG_INFO("r2s log info");
  R2S_LOG_WARN("r2s log warn");
  R2S_LOG_ERROR("r2s log error");
  R2S_LOG_FATAL("r2s log fatal");
}

static void sleep_1ms(void)
{
  const struct timespec req = {0, 1000000L};

  (void)nanosleep(&req, NULL);
}

static void parse_options(int argc, char **argv, struct options *opts)
{
  int i;

  opts->port = argv[1];
  opts->node = "r2s_posix";
  opts->pub = kDefaultPub;
  opts->sub = kDefaultSub;
  opts->echo = kDefaultEcho;
  opts->log_text = kDefaultLogText;
  opts->time_now = kDefaultTimeNow;
  opts->skew_ppm = 0;
  opts->rate = 10u;
  opts->log_lines = 0u;
  for (i = 2; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--node") == 0) {
      opts->node = take_value(argc, argv, &i, arg);
    } else if (strcmp(arg, "--rate") == 0) {
      opts->rate = take_rate(take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--pub") == 0) {
      reject_list_change(arg, take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--sub") == 0) {
      reject_list_change(arg, take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--echo") == 0) {
      reject_list_change(arg, take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--log-text") == 0) {
      reject_list_change(arg, take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--time-now") == 0) {
      reject_list_change(arg, take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--joints") == 0) {
      reject_list_change(arg, take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--tick-skew-ppm") == 0) {
      opts->skew_ppm = take_skew(take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--no-pub") == 0) {
      reject_list_change(arg, kDefaultPub);
    } else if (strcmp(arg, "--no-sub") == 0) {
      reject_list_change(arg, kDefaultSub);
    } else if (strcmp(arg, "--no-echo") == 0) {
      reject_list_change(arg, kDefaultEcho);
    } else if (strcmp(arg, "--no-log-text") == 0) {
            /* The announce keeps the fixed rows, so the flag silences the
             * log_text traffic instead of dropping the topic. */
      opts->log_text = NULL;
    } else if (strcmp(arg, "--no-time-now") == 0) {
      reject_list_change(arg, kDefaultTimeNow);
    } else if (strcmp(arg, "--no-joints") == 0) {
      reject_list_change(arg, "joints");
    } else if (strcmp(arg, "--log-lines") == 0) {
      opts->log_lines = 1u;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout);
      exit(0);
    } else {
      fprintf(stderr, "r2s_posix: unknown option '%s'\n", arg);
      usage(stderr);
      exit(2);
    }
  }
  check_node_name("--node", opts->node);
}

int main(int argc, char **argv)
{
  struct options opts;
  uint32_t interval;
  uint32_t last_pub;
  uint32_t last_time;
  uint32_t last_log = 0u;
  uint32_t sent = 0u;
  uint8_t log_booted = 0u;

  setvbuf(stderr, NULL, _IONBF, 0);
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    usage(stdout);
    return 0;
  }
  parse_options(argc, argv, &opts);
  r2s_posix_set_tick_skew_ppm(opts.skew_ppm);
  if (r2s_posix_open(opts.port) < 0) {
    fprintf(stderr, "r2s_posix: cannot open '%s': %s\n", opts.port, strerror(errno));
    return 1;
  }
    /* The node names the session at runtime; the topics stay the compiled
     * rows. The option probe above refused an overlong name, so the first
     * spin binds the static topic table instead of meeting it unconnected. */
  static r2s::Node node(opts.node);
  node.spin_some();
  fprintf(stderr, "r2s_posix: ready node=%s rate=%u\n", opts.node, opts.rate);
  interval = (uint32_t)(1000u / opts.rate);
  last_pub = node.now();
  last_time = last_pub;
  for (;; ) {
    uint32_t now;

    node.spin_some();
    now = node.now();
    if (node.connected() == 0) {
            /* Offline there are no samples, so the clock resyncs instead of
             * queueing a burst for the reconnect. */
      last_pub = now;
      last_time = now;
      sleep_1ms();
      continue;
    }
    if (opts.log_lines != 0u && (uint32_t)(now - last_log) >= LOG_PERIOD_MS) {
      emit_log_lines();
      last_log = now;
    }
    if (opts.log_text != NULL && log_booted == 0u) {
            /* A boot line that fails to publish is retried until it sends.
             * It carries the assigned session token, so the host sees what
             * the firmware stored. */
      if (publish_log_text(r2s_session_token()) != 0) {
        log_booted = 1u;
      }
    }
        /* Every missed tick is still owed: one frame per missed interval
         * keeps the count whole when the host steals the firmware tick. */
    while ((uint32_t)(now - last_pub) >= interval) {
      std_msgs::msg::Float32 msg;

      msg.data = (float)sent;
      if (battery_pub.publish(msg) != R2S_OK) {
        break;
      }
      sent++;
      last_pub += interval;
      if (opts.log_text != NULL && sent % 10u == 0u) {
        (void)publish_log_text(sent);
      }
    }
    if (r2s_time_synced() != 0u) {
            /* One stamp per interval, no catch-up: a stalled link skips
             * samples the host would read as the past. */
      if ((uint32_t)(now - last_time) >= interval) {
        builtin_interfaces::msg::Time stamp;
        r2s_time_t anchored = r2s_now();

        stamp.sec = anchored.sec;
        stamp.nanosec = anchored.nanosec;
        if (time_now_pub.publish(stamp) == R2S_OK) {
          last_time = now;
        }
      }
    }
  }
  return 0;
}
