// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* The compat firmware: the runtime firmware's topics and timing through
 * the rosserial-compatible layer, so the e2e suite also covers the compat
 * path. Publish intervals come from the monotonic clock rather than the
 * skewed port tick, so they stay true-time under --tick-skew-ppm; the
 * stamps ride the shared clock through nh.now(), like the runtime. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "r2s/runtime.hpp"
#include "r2s_posix_port.h"
#include "ros2serial/builtin_interfaces/Time.h"
#include "ros2serial/geometry_msgs/Twist.h"
#include "ros2serial/ros.h"
#include "ros2serial/sensor_msgs/JointState.h"
#include "ros2serial/std_msgs/Float32.h"
#include "ros2serial/std_msgs/String.h"

namespace
{

#define RATE_MIN 1u
#define RATE_MAX 100u
#define SKEW_MIN_PPM -20000
#define SKEW_MAX_PPM 20000
#define FLOOD_MAX_PER_PASS 64u
#define FLOOD_REPORT_MS 2000u
#define LOG_PERIOD_MS 2000u

/* The tick as milliseconds, from the monotonic clock. */
uint64_t now_ms()
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

void sleep_1ms()
{
  const struct timespec req = {0, 1000000L};

  (void)nanosleep(&req, NULL);
}

struct Options
{
  const char *device;
  const char *node;
  const char *pub;
  const char *sub;
  const char *echo;
  const char *log_text;
  const char *time_now;
  const char *joints;
  int32_t skew_ppm;
  unsigned rate;
  uint16_t tx_cap;
  unsigned log_lines;
  bool flood;
};

/* The echo publisher the callback reaches: set once the topics advertise, so
 * a frame that arrives before the table closes has nowhere to go. */
ros::Publisher *s_echo = 0;

uint32_t s_cmd_vel_count = 0;

void on_cmd_vel(const geometry_msgs::Twist & msg)
{
  s_cmd_vel_count++;
  fprintf(stderr, "r2s_posix: cmd_vel n=%lu\n", static_cast<unsigned long>(s_cmd_vel_count));
    /* Best effort echo, as in the runtime firmware. */
  if (s_echo != 0) {
    (void)s_echo->publish(&msg);
  }
}

uint32_t s_joints_count = 0;

void on_joints(const sensor_msgs::JointState & msg)
{
  (void)msg;
  s_joints_count++;
  fprintf(stderr, "r2s_posix: joints n=%lu\n", static_cast<unsigned long>(s_joints_count));
}

void usage(FILE *out, const char *argv0)
{
  fprintf(out,
            "Usage: %s DEVICE [--node NAME] [--rate HZ]\n"
            "           [--pub NAME | --no-pub] [--sub NAME | --no-sub]\n"
            "           [--echo NAME | --no-echo] [--time-now NAME | --no-time-now]\n"
            "           [--joints NAME | --no-joints]\n"
            "           [--log-text NAME | --no-log-text] [--log-lines] [--tick-skew-ppm PPM]\n"
            "           [--tx-free N] [--flood]\n",
            argv0);
}

const char * take_value(int argc, char **argv, int *i, const char *flag)
{
  if (*i + 1 >= argc) {
    fprintf(stderr, "r2s_posix: %s needs a value\n", flag);
    usage(stderr, argv[0]);
    exit(2);
  }
  (*i)++;
  return argv[*i];
}

unsigned take_rate(const char *text, const char *argv0)
{
  char *end = 0;
  long rate = 0;

  errno = 0;
  rate = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || rate < (long)RATE_MIN ||
    rate > (long)RATE_MAX)
  {
    fprintf(stderr, "r2s_posix: --rate needs %u..%u, got '%s'\n", RATE_MIN, RATE_MAX, text);
    usage(stderr, argv0);
    exit(2);
  }
  return static_cast<unsigned>(rate);
}

int32_t take_skew(const char *text, const char *argv0)
{
  char *end = 0;
  long skew = 0;

  errno = 0;
  skew = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || skew < (long)SKEW_MIN_PPM ||
    skew > (long)SKEW_MAX_PPM)
  {
    fprintf(stderr, "r2s_posix: --tick-skew-ppm needs %d..%d, got '%s'\n", SKEW_MIN_PPM,
        SKEW_MAX_PPM,
                text);
    usage(stderr, argv0);
    exit(2);
  }
  return static_cast<int32_t>(skew);
}

void check_name(const char *flag, const char *name, const char *argv0)
{
  if (name[0] == '\0') {
    fprintf(stderr, "r2s_posix: %s needs a non-empty name\n", flag);
    usage(stderr, argv0);
    exit(2);
  }
}

/* The per-tick byte budget: 1 to 65534, since 0 would wedge the link and
 * 0xFFFF is the uncapped value the flag's absence keeps. */
uint16_t take_tx_cap(const char *text, const char *argv0)
{
  char *end = 0;
  long cap = 0;

  errno = 0;
  cap = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || cap < 1L || cap > 65534L) {
    fprintf(stderr, "r2s_posix: --tx-free needs 1..65534, got '%s'\n", text);
    usage(stderr, argv0);
    exit(2);
  }
  return static_cast<uint16_t>(cap);
}

/* One log_text line: the value as 8 hex digits. The bundled String bound
 * holds 16 characters, so the 8 digits fit with room to spare. */
bool publish_log_text(ros::Publisher *pub, uint32_t value)
{
  std_msgs::msg::String msg;
  uint8_t i = 0;

  memset(&msg, 0, sizeof msg);
  for (i = 0u; i < 8u; i++) {
    int digit = static_cast<int>((value >> 28u) & 0xFu);

    msg.data[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    value <<= 4u;
  }
  msg.data[8] = '\0';
  return pub->publish(&msg) == 0;
}

/* One line per level every LOG_PERIOD_MS while connected, so the host log
 * test sees every severity. Plain text, which the bridge renders verbatim. */
void emit_log_lines(ros::NodeHandle & nh)
{
  nh.logdebug("r2s log debug");
  nh.loginfo("r2s log info");
  nh.logwarn("r2s log warn");
  nh.logerror("r2s log error");
  nh.logfatal("r2s log fatal");
}

} // namespace

int main(int argc, char **argv)
{
  Options opts;
  int i = 2;
  uint64_t interval = 0u;
  uint64_t last_pub = 0u;
  uint64_t last_time = 0u;
  uint64_t last_log = 0u;
  uint64_t last_flood = 0u;
  uint32_t sent = 0u;
  uint32_t busy = 0u;
  unsigned log_booted = 0u;
  unsigned cap_armed = 0u;

  setvbuf(stderr, NULL, _IONBF, 0);
  if (argc < 2) {
    usage(stderr, argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    usage(stdout, argv[0]);
    return 0;
  }
  opts.device = argv[1];
    /* The compat layer names no node: sketches announce as rosserial, so the
     * flag is accepted and ignored. */
  opts.node = "rosserial";
  opts.pub = "battery";
  opts.sub = "cmd_vel";
  opts.echo = "echo_vel";
  opts.log_text = "log_text";
  opts.time_now = "time_now";
  opts.joints = "joints";
  opts.skew_ppm = 0;
  opts.rate = 10u;
  opts.tx_cap = 0xFFFFu;
  opts.log_lines = 0u;
  opts.flood = false;
  for (; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--node") == 0) {
            /* The compat layer names no node: the value is validated like
             * the runtime's, then ignored, since sketches announce as
             * rosserial. */
      check_name(arg, take_value(argc, argv, &i, arg), argv[0]);
    } else if (strcmp(arg, "--rate") == 0) {
      opts.rate = take_rate(take_value(argc, argv, &i, arg), argv[0]);
    } else if (strcmp(arg, "--pub") == 0) {
      opts.pub = take_value(argc, argv, &i, arg);
      check_name(arg, opts.pub, argv[0]);
    } else if (strcmp(arg, "--sub") == 0) {
      opts.sub = take_value(argc, argv, &i, arg);
      check_name(arg, opts.sub, argv[0]);
    } else if (strcmp(arg, "--echo") == 0) {
      opts.echo = take_value(argc, argv, &i, arg);
      check_name(arg, opts.echo, argv[0]);
    } else if (strcmp(arg, "--log-text") == 0) {
      opts.log_text = take_value(argc, argv, &i, arg);
      check_name(arg, opts.log_text, argv[0]);
    } else if (strcmp(arg, "--time-now") == 0) {
      opts.time_now = take_value(argc, argv, &i, arg);
      check_name(arg, opts.time_now, argv[0]);
    } else if (strcmp(arg, "--joints") == 0) {
      opts.joints = take_value(argc, argv, &i, arg);
      check_name(arg, opts.joints, argv[0]);
    } else if (strcmp(arg, "--tick-skew-ppm") == 0) {
      opts.skew_ppm = take_skew(take_value(argc, argv, &i, arg), argv[0]);
    } else if (strcmp(arg, "--tx-free") == 0) {
      opts.tx_cap = take_tx_cap(take_value(argc, argv, &i, arg), argv[0]);
    } else if (strcmp(arg, "--flood") == 0) {
      opts.flood = true;
    } else if (strcmp(arg, "--no-pub") == 0) {
      opts.pub = 0;
    } else if (strcmp(arg, "--no-sub") == 0) {
      opts.sub = 0;
    } else if (strcmp(arg, "--no-echo") == 0) {
      opts.echo = 0;
    } else if (strcmp(arg, "--no-log-text") == 0) {
      opts.log_text = 0;
    } else if (strcmp(arg, "--no-time-now") == 0) {
      opts.time_now = 0;
    } else if (strcmp(arg, "--no-joints") == 0) {
      opts.joints = 0;
    } else if (strcmp(arg, "--log-lines") == 0) {
      opts.log_lines = 1u;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout, argv[0]);
      return 0;
    } else {
      fprintf(stderr, "r2s_posix: unknown option '%s'\n", arg);
      usage(stderr, argv[0]);
      return 2;
    }
  }
  if (opts.flood && opts.tx_cap == 0xFFFFu) {
    fprintf(stderr, "r2s_posix: --flood needs --tx-free\n");
    usage(stderr, argv[0]);
    return 2;
  }
  r2s_posix_set_tick_skew_ppm(opts.skew_ppm);
  if (r2s_posix_open(opts.device) < 0) {
    fprintf(stderr, "r2s_posix: cannot open '%s': %s\n", opts.device, strerror(errno));
    return 1;
  }
  {
    ros::NodeHandle nh;
    std_msgs::msg::Float32 battery_msg;
    geometry_msgs::msg::Twist echo_msg;
    std_msgs::msg::String log_msg;
    builtin_interfaces::msg::Time time_msg;
    ros::Publisher battery_pub(opts.pub != 0 ? opts.pub : "", &battery_msg);
    ros::Publisher echo_pub(opts.echo != 0 ? opts.echo : "", &echo_msg);
    ros::Publisher log_pub(opts.log_text != 0 ? opts.log_text : "", &log_msg);
    ros::Publisher time_pub(opts.time_now != 0 ? opts.time_now : "", &time_msg);
    ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub(opts.sub != 0 ? opts.sub : "", &on_cmd_vel);
    ros::Subscriber<sensor_msgs::JointState> joints_sub(opts.joints != 0 ? opts.joints : "",
      &on_joints);
    bool configured = true;

    (void)opts.node;
    memset(&battery_msg, 0, sizeof battery_msg);
    memset(&echo_msg, 0, sizeof echo_msg);
    memset(&log_msg, 0, sizeof log_msg);
    memset(&time_msg, 0, sizeof time_msg);
    nh.initNode();
    if (opts.pub != 0) {
      configured = nh.advertise(battery_pub) && configured;
    }
    if (opts.sub != 0) {
      configured = nh.subscribe(cmd_vel_sub) && configured;
    }
    if (opts.echo != 0) {
      configured = nh.advertise(echo_pub) && configured;
      s_echo = &echo_pub;
    }
    if (opts.log_text != 0) {
      configured = nh.advertise(log_pub) && configured;
    }
    if (opts.time_now != 0) {
      configured = nh.advertise(time_pub) && configured;
    }
    if (opts.joints != 0) {
      configured = nh.subscribe(joints_sub) && configured;
    }
    if (!configured) {
      fprintf(stderr, "r2s_posix: cannot configure node '%s'\n", opts.node);
      return 2;
    }
    fprintf(stderr, "r2s_posix: ready node=%s rate=%u\n", opts.node, opts.rate);
    interval = static_cast<uint64_t>(1000u / opts.rate);
    last_pub = now_ms();
    last_time = last_pub;
    for (;; ) {
      uint64_t now = 0u;

      nh.spinOnce();
      now = now_ms();
      if (!nh.connected()) {
                /* Offline there are no samples, so the clock resyncs instead
                 * of queueing a burst for the reconnect. */
        last_pub = now;
        last_time = now;
        if (cap_armed != 0u) {
          cap_armed = 0u;
          r2s_posix_set_tx_cap(0xFFFFu);
        }
        sleep_1ms();
        continue;
      }
      if (opts.tx_cap != 0xFFFFu && cap_armed == 0u) {
                /* The cap shapes steady-state traffic only: the handshake
                 * ran uncapped, so its longer frames always fit. */
        r2s_posix_set_tx_cap(opts.tx_cap);
        cap_armed = 1u;
      }
      if (opts.log_lines != 0u && (now - last_log) >= LOG_PERIOD_MS) {
        emit_log_lines(nh);
        last_log = now;
      }
      if (opts.log_text != 0 && log_booted == 0u) {
                /* A boot line that fails to publish is retried until it
                 * sends. It carries the assigned session token, so the host
                 * sees what the firmware stored. */
        if (publish_log_text(&log_pub, r2s_session_token())) {
          log_booted = 1u;
        }
      }
      if (opts.pub != 0) {
        if (opts.flood) {
                    /* Saturate the link: publish until the port refuses, one
                     * sample per attempt so the shed samples show as gaps in
                     * the received values. Bounded per pass, so a build that
                     * never refuses still returns to its tick. */
          uint16_t n = 0u;

          for (n = 0u; n < FLOOD_MAX_PER_PASS; n++) {
            battery_msg.data = static_cast<float>(sent);
            sent++;
            if (battery_pub.publish(&battery_msg) == 0) {
              continue;
            }
            busy++;
            break;
          }
          if ((now - last_flood) >= FLOOD_REPORT_MS) {
            fprintf(stderr, "r2s_posix: flood sent=%lu busy=%lu\n",
                                static_cast<unsigned long>(sent), static_cast<unsigned long>(busy));
            last_flood = now;
          }
        } else {
                    /* Every missed tick is still owed: one frame per missed
                     * interval keeps the count whole when the host steals the
                     * firmware tick. */
          while ((now - last_pub) >= interval) {
            battery_msg.data = static_cast<float>(sent);
            if (battery_pub.publish(&battery_msg) != 0) {
              break;
            }
            sent++;
            last_pub += interval;
            if (opts.log_text != 0 && sent % 10u == 0u) {
              (void)publish_log_text(&log_pub, sent);
            }
          }
        }
      } else {
        sleep_1ms();
      }
      if (opts.time_now != 0 && r2s_time_synced() != 0u) {
                /* One stamp per interval, no catch-up: a stalled link skips
                 * samples the host would read as the past. */
        if ((now - last_time) >= interval) {
          ros::Time stamp = nh.now();

          time_msg.sec = static_cast<int32_t>(stamp.sec);
          time_msg.nanosec = stamp.nsec;
          if (time_pub.publish(&time_msg) == 0) {
            last_time = now;
          }
        }
      }
    }
  }
}
