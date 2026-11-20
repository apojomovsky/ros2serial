// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Services posix firmware: the runtime firmware's topics and timing plus the
 * services channel, so the end-to-end suite drives r2s::Service and r2s::Client
 * against a ROS client. The firmware serves reset, and with --trigger calls
 * trigger (both std_srvs Trigger) and reports the call's outcome on log_text.
 * Publish intervals come from the monotonic clock rather than the skewed port
 * tick, so they stay true-time under --tick-skew-ppm. */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ros2serial.h"

#include <builtin_interfaces/msg/time.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/string.hpp>

#include <geometry_msgs/msg/twist.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "r2s/r2s.h"
#include "r2s/r2s_session.h"
#include "r2s/service.hpp"

#include "r2s_posix_params.h"
#include "r2s_posix_port.h"

/* Publish rates in Hz, from once a second to the loop's 1 ms granularity. */
#define RATE_MIN 1u
#define RATE_MAX 100u

/* One log line per this many ms while connected, so the log test sees the
 * whole severity range. */
#define LOG_PERIOD_MS 2000u

/* How long to wait before retrying the MCU's call: the bridge builds its
 * client route at the handshake, so a call that beats it comes back refused
 * and the next attempt finds the route. */
#define TRIGGER_RETRY_MS 250u

/* The skew range the wire correction carries (spec/protocol.md [TIM-4]):
 * past it the host must refuse the correction, so the knob stops here. */
#define SKEW_MIN_PPM (-20000)
#define SKEW_MAX_PPM 20000

/* Default topic names the command-line flags rename. */
static const char *const kDefaultPub = "battery";
static const char *const kDefaultSub = "cmd_vel";
static const char *const kDefaultEcho = "echo_vel";
static const char *const kDefaultLogText = "log_text";
static const char *const kDefaultTimeNow = "time_now";

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
  uint8_t trigger;
};

/* The echo publisher the callback reaches: set once the topics register, so
 * a frame that arrives before the table closes has nowhere to go. */
static r2s::Publisher<geometry_msgs::msg::Twist> *s_echo;

static uint32_t s_cmd_vel_count;

void on_cmd_vel(const geometry_msgs::msg::Twist & msg)
{
  s_cmd_vel_count++;
  fprintf(stderr, "r2s_posix: cmd_vel n=%lu\n", (unsigned long)s_cmd_vel_count);
    /* Best effort echo, as in the runtime firmware. */
  if (s_echo != NULL) {
    (void)s_echo->publish(msg);
  }
}

static uint32_t s_joints_count;

void on_joints(const sensor_msgs::msg::JointState & msg)
{
  (void)msg;
  s_joints_count++;
  fprintf(stderr, "r2s_posix: joints n=%lu\n", (unsigned long)s_joints_count);
}

/* Answers reset the way the C firmware does: success with the MCU's own
 * text, so the ROS call proves the round trip carries MCU content. */
static void on_reset(
  const std_srvs::srv::Trigger::Request & req, std_srvs::srv::Trigger::Response & res)
{
  (void)req;
  res.success = 1u;
  (void)snprintf(res.message, sizeof res.message, "%s", "ok");
}

static uint64_t now_ms()
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000u +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

static void sleep_1ms()
{
  const struct timespec req = {0, 1000000L};

  (void)nanosleep(&req, NULL);
}

static void usage(FILE *out)
{
  fprintf(out, "Usage: r2s_posix_services_firmware PORT [--node NAME] [--rate HZ]\n"
                 "           [--pub NAME | --no-pub] [--sub NAME | --no-sub]\n"
                 "           [--echo NAME | --no-echo] [--time-now NAME | --no-time-now]\n"
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
  char *end = NULL;
  long rate = 0;

  errno = 0;
  rate = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || rate < (long)RATE_MIN ||
    rate > (long)RATE_MAX)
  {
    fprintf(stderr, "r2s_posix: --rate needs %u..%u, got '%s'\n", RATE_MIN, RATE_MAX, text);
    usage(stderr);
    exit(2);
  }
  return static_cast<unsigned>(rate);
}

static int32_t take_skew(const char *text)
{
  char *end = NULL;
  long skew = 0;

  errno = 0;
  skew = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || skew < (long)SKEW_MIN_PPM ||
    skew > (long)SKEW_MAX_PPM)
  {
    fprintf(stderr, "r2s_posix: --tick-skew-ppm needs %d..%d, got '%s'\n", SKEW_MIN_PPM,
        SKEW_MAX_PPM, text);
    usage(stderr);
    exit(2);
  }
  return static_cast<int32_t>(skew);
}

static void check_name(const char *flag, const char *name)
{
  if (name[0] == '\0') {
    fprintf(stderr, "r2s_posix: %s needs a non-empty name\n", flag);
    usage(stderr);
    exit(2);
  }
}

/* One log_text line: the value as 8 hex digits. The bundled String bound
 * holds 16 characters, so the 8 digits fit with room to spare. */
static bool publish_log_text(r2s::Publisher<std_msgs::msg::String> *pub, uint32_t value)
{
  std_msgs::msg::String msg;

  for (uint8_t i = 0u; i < 8u; i++) {
    int digit = static_cast<int>((value >> 28u) & 0xFu);

    msg.data[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    value <<= 4u;
  }
  msg.data[8] = '\0';
  return pub->publish(msg) == R2S_OK;
}

/* One line per level every LOG_PERIOD_MS while connected, so the host log
 * test sees every severity. Plain text, which the bridge renders verbatim. */
static void emit_log_lines()
{
  r2s_log_debug_ram("r2s log debug");
  r2s_log_info_ram("r2s log info");
  r2s_log_warn_ram("r2s log warn");
  r2s_log_error_ram("r2s log error");
  r2s_log_fatal_ram("r2s log fatal");
}

static void parse_options(int argc, char **argv, struct options *opts)
{
  int i = 2;

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
  opts->trigger = 0u;
  for (; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--node") == 0) {
      opts->node = take_value(argc, argv, &i, arg);
    } else if (strcmp(arg, "--rate") == 0) {
      opts->rate = take_rate(take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--pub") == 0) {
      opts->pub = take_value(argc, argv, &i, arg);
      check_name(arg, opts->pub);
    } else if (strcmp(arg, "--sub") == 0) {
      opts->sub = take_value(argc, argv, &i, arg);
      check_name(arg, opts->sub);
    } else if (strcmp(arg, "--echo") == 0) {
      opts->echo = take_value(argc, argv, &i, arg);
      check_name(arg, opts->echo);
    } else if (strcmp(arg, "--log-text") == 0) {
      opts->log_text = take_value(argc, argv, &i, arg);
      check_name(arg, opts->log_text);
    } else if (strcmp(arg, "--time-now") == 0) {
      opts->time_now = take_value(argc, argv, &i, arg);
      check_name(arg, opts->time_now);
    } else if (strcmp(arg, "--tick-skew-ppm") == 0) {
      opts->skew_ppm = take_skew(take_value(argc, argv, &i, arg));
    } else if (strcmp(arg, "--no-pub") == 0) {
      opts->pub = NULL;
    } else if (strcmp(arg, "--no-sub") == 0) {
      opts->sub = NULL;
    } else if (strcmp(arg, "--no-echo") == 0) {
      opts->echo = NULL;
    } else if (strcmp(arg, "--no-log-text") == 0) {
      opts->log_text = NULL;
    } else if (strcmp(arg, "--no-time-now") == 0) {
      opts->time_now = NULL;
    } else if (strcmp(arg, "--log-lines") == 0) {
      opts->log_lines = 1u;
    } else if (strcmp(arg, "--trigger") == 0) {
      opts->trigger = 1u;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout);
      exit(0);
    } else {
      fprintf(stderr, "r2s_posix: unknown option '%s'\n", arg);
      usage(stderr);
      exit(2);
    }
  }
  check_name("--node", opts->node);
}

int main(int argc, char **argv)
{
  struct options opts;
  uint64_t interval;
  uint64_t last_pub;
  uint64_t last_time;
  uint64_t last_log = 0u;
  uint32_t sent = 0u;
  uint8_t log_booted = 0u;
  uint8_t trigger_called = 0u;
  uint64_t trigger_last_try = 0u;

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
    /* Function-local objects, so the command line is parsed before anything
     * registers: a dropped topic is never constructed. The services carry
     * their own records, so they live as long as the node spins. */
  static r2s::Node node(opts.node);
  r2s::Publisher<std_msgs::msg::Float32> *battery_pub = NULL;
  r2s::Publisher<std_msgs::msg::String> *log_pub = NULL;
  r2s::Publisher<builtin_interfaces::msg::Time> *time_pub = NULL;
  if (opts.pub != NULL) {
    static r2s::Publisher<std_msgs::msg::Float32> pub(node, opts.pub);
    battery_pub = &pub;
  }
  if (opts.sub != NULL) {
    static r2s::Subscription<geometry_msgs::msg::Twist> sub(node, opts.sub, on_cmd_vel);
  }
  if (opts.echo != NULL) {
    static r2s::Publisher<geometry_msgs::msg::Twist> echo(node, opts.echo);
    s_echo = &echo;
  }
  if (opts.log_text != NULL) {
    static r2s::Publisher<std_msgs::msg::String> log_text(node, opts.log_text);
    log_pub = &log_text;
  }
  if (opts.time_now != NULL) {
    static r2s::Publisher<builtin_interfaces::msg::Time> time_now(node, opts.time_now);
    time_pub = &time_now;
  }
  static r2s::Service<std_srvs::srv::Trigger> reset_srv(node, "reset", on_reset);
  static r2s::Client<std_srvs::srv::Trigger> trigger_cli(node, "trigger");
    /* The first spin binds the table the objects registered: a name the
     * announce cannot hold refuses here, before the ready line, instead of
     * failing the handshake with no detail. */
  node.spin_some();
  if (node.registration_error() != 0 || reset_srv.index() == 0xFFu ||
    trigger_cli.index() == 0xFFu)
  {
    fprintf(stderr, "r2s_posix: cannot configure node '%s'\n", opts.node);
    return 2;
  }
    /* The parameter table the bridge walks after the topics. */
  if (r2s_posix_params_init() != 0) {
    fprintf(stderr, "r2s_posix: cannot configure parameters\n");
    return 2;
  }
  fprintf(stderr, "r2s_posix: ready node=%s rate=%u\n", opts.node, opts.rate);
  interval = static_cast<uint64_t>(1000u / opts.rate);
  last_pub = now_ms();
  last_time = last_pub;
  for (;; ) {
    uint64_t now = 0u;

    node.spin_some();
    now = now_ms();
    if (!node.connected()) {
            /* Offline there are no samples, so the clock resyncs instead of
             * queueing a burst for the reconnect. The call restarts, since
             * the host forgets an outstanding request with the session. */
      last_pub = now;
      last_time = now;
      trigger_called = 0u;
      trigger_last_try = 0u;
      sleep_1ms();
      continue;
    }
    if (opts.log_lines != 0u && (now - last_log) >= LOG_PERIOD_MS) {
      emit_log_lines();
      last_log = now;
    }
    if (opts.log_text != NULL && log_booted == 0u) {
            /* A boot line that fails to publish is retried until it sends.
             * It carries the assigned session token, so the host sees what
             * the firmware stored. */
      if (publish_log_text(log_pub, r2s_session_token())) {
        log_booted = 1u;
      }
    }
        /* Applied SETs print here, so the suite reads the live values. */
    r2s_posix_params_report();
    if (opts.trigger != 0u && trigger_called == 0u) {
            /* The call blocks until its answer, a refusal or the call
             * timeout, so the loop stalls while it waits: a firmware that
             * publishes on a schedule leaves this off rather than paying
             * that stall on every connection. A refused call retries on a
             * spacing, since the bridge builds its client route at the
             * handshake. The answer's marker goes out on log_text, which is
             * how the service scenario observes the round trip. */
      if (trigger_last_try == 0u || (now - trigger_last_try) >= TRIGGER_RETRY_MS) {
        std_srvs::srv::Trigger::Request req;
        std_srvs::srv::Trigger::Response res;

        trigger_last_try = now;
        req.structure_needs_at_least_one_member = 0u;
        if (trigger_cli.call(req, res)) {
          trigger_called = 1u;
          if (opts.log_text != NULL) {
            (void)publish_log_text(log_pub, 0x5E000000u);
          }
        }
      }
    }
    if (battery_pub != NULL) {
            /* Every missed tick is still owed: one frame per missed interval
             * keeps the count whole when the host steals the firmware tick. */
      while ((now - last_pub) >= interval) {
        std_msgs::msg::Float32 msg;

        msg.data = static_cast<float>(sent);
        if (battery_pub->publish(msg) != R2S_OK) {
          break;
        }
        sent++;
        last_pub += interval;
        if (opts.log_text != NULL && sent % 10u == 0u) {
          (void)publish_log_text(log_pub, sent);
        }
      }
    } else {
      sleep_1ms();
    }
    if (time_pub != NULL && r2s_time_synced() != 0u) {
            /* One stamp per interval, no catch-up: a stalled link skips
             * samples the host would read as the past. */
      if ((now - last_time) >= interval) {
        builtin_interfaces::msg::Time stamp;
        r2s_time_t anchored = r2s_now();

        stamp.sec = anchored.sec;
        stamp.nanosec = anchored.nanosec;
        if (time_pub->publish(stamp) == R2S_OK) {
          last_time = now;
        }
      }
    }
  }
  return 0;
}
