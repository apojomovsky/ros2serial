/* Runtime posix firmware: the same Float32, Twist, String and Time topics as
 * the generated C firmware, registered as r2s::Node objects while the
 * firmware runs, so the end-to-end suite drives the runtime registration path
 * as a process on a pty. The command line mirrors the C firmware's, and the
 * topics register in the same order with IDs from 16, so a dropped topic
 * is never registered and later topics shift up. The IDs live only in one
 * announce, so either numbering is consistent on the wire. */

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

#include "r2s/r2s.h"
#include "r2s/r2s_session.h"

#include "r2s_posix_port.h"

/* Publish rates in Hz, from once a second to the loop's 1 ms granularity. */
#define RATE_MIN 1u
#define RATE_MAX 100u

/* A flood pass publishes at most this many samples before returning to the
 * tick, and reports its cumulative totals at most this often in ms. */
#define FLOOD_MAX_PER_PASS 64u
#define FLOOD_REPORT_MS 2000u

/* Default topic names the command-line flags rename. */
static const char *const kDefaultPub = "battery";
static const char *const kDefaultSub = "cmd_vel";
static const char *const kDefaultEcho = "echo_vel";
static const char *const kDefaultLogText = "log_text";
static const char *const kDefaultTimeNow = "time_now";
static const char *const kDefaultJoints = "joints";

/* The skew range the wire correction carries (spec/protocol.md [TIM-4]):
 * past it the host must refuse the correction, so the knob stops here. */
#define SKEW_MIN_PPM (-20000)
#define SKEW_MAX_PPM 20000

struct options {
    const char *port;
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
    uint8_t log_lines;
    uint8_t flood;
};

/* The echo publisher the callback reaches: set once the topics register, so
 * a frame that arrives before the table closes has nowhere to go. */
static r2s::Publisher<geometry_msgs::msg::Twist> *s_echo;

static uint32_t s_cmd_vel_count;

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    s_cmd_vel_count++;
    fprintf(stderr, "r2s_posix: cmd_vel n=%lu\n", (unsigned long)s_cmd_vel_count);
    /* Best effort echo for scenario 3: five messages on an idle link never
     * fill the port, so the return carries no signal the test would use. */
    if (s_echo != NULL) {
        (void)s_echo->publish(msg);
    }
}

static uint32_t s_joints_count;

void on_joints(const sensor_msgs::msg::JointState &msg)
{
    (void)msg;
    s_joints_count++;
    fprintf(stderr, "r2s_posix: joints n=%lu\n", (unsigned long)s_joints_count);
}

static void usage(FILE *out)
{
    fprintf(out, "Usage: r2s_posix_runtime_firmware PORT [--node NAME] [--rate HZ]\n"
                 "           [--pub NAME | --no-pub] [--sub NAME | --no-sub]\n"
                 "           [--echo NAME | --no-echo] [--time-now NAME | --no-time-now]\n"
                 "           [--joints NAME | --no-joints]\n"
                 "           [--log-text NAME | --no-log-text] [--log-lines] [--tick-skew-ppm PPM]\n"
                 "           [--tx-free N] [--flood]\n");
}

static const char *take_value(int argc, char **argv, int *i, const char *flag)
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
    if (end == text || *end != '\0' || errno == ERANGE || rate < (long)RATE_MIN || rate > (long)RATE_MAX) {
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
        skew > (long)SKEW_MAX_PPM) {
        fprintf(stderr, "r2s_posix: --tick-skew-ppm needs %d..%d, got '%s'\n", SKEW_MIN_PPM, SKEW_MAX_PPM,
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

/* The per-tick byte budget: 1 to 65534, since 0 would wedge the link and
 * 0xFFFF is the uncapped value the flag's absence keeps. */
static uint16_t take_tx_cap(const char *text)
{
    char *end;
    long cap;

    errno = 0;
    cap = strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || cap < 1L || cap > 65534L) {
        fprintf(stderr, "r2s_posix: --tx-free needs 1..65534, got '%s'\n", text);
        usage(stderr);
        exit(2);
    }
    return static_cast<uint16_t>(cap);
}

/* One log_text line: the value as 8 hex digits. The bundled String bound
 * holds 16 characters, so the 8 digits fit with room to spare. */
static bool publish_log_text(r2s::Publisher<std_msgs::msg::String> *pub, uint32_t value)
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
    return pub->publish(msg) == R2S_OK;
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
    opts->joints = kDefaultJoints;
    opts->skew_ppm = 0;
    opts->rate = 10u;
    opts->tx_cap = 0xFFFFu;
    opts->log_lines = 0u;
    opts->flood = 0u;
    for (i = 2; i < argc; i++) {
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
        } else if (strcmp(arg, "--joints") == 0) {
            opts->joints = take_value(argc, argv, &i, arg);
            check_name(arg, opts->joints);
        } else if (strcmp(arg, "--tick-skew-ppm") == 0) {
            opts->skew_ppm = take_skew(take_value(argc, argv, &i, arg));
        } else if (strcmp(arg, "--tx-free") == 0) {
            opts->tx_cap = take_tx_cap(take_value(argc, argv, &i, arg));
        } else if (strcmp(arg, "--flood") == 0) {
            opts->flood = 1u;
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
        } else if (strcmp(arg, "--no-joints") == 0) {
            opts->joints = NULL;
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
    if (opts->flood != 0u && opts->tx_cap == 0xFFFFu) {
        fprintf(stderr, "r2s_posix: --flood needs --tx-free\n");
        usage(stderr);
        exit(2);
    }
    check_name("--node", opts->node);
}

int main(int argc, char **argv)
{
    struct options opts;
    uint32_t interval;
    uint32_t last_pub;
    uint32_t last_time;
    uint32_t last_log = 0u;
    uint32_t last_flood = 0u;
    uint32_t sent = 0u;
    uint32_t busy = 0u;
    uint8_t log_booted = 0u;
    uint8_t cap_armed = 0u;

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
     * registers: a dropped topic is never constructed. */
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
    if (opts.joints != NULL) {
        static r2s::Subscription<sensor_msgs::msg::JointState> joints(node, opts.joints, on_joints);
    }
    /* The first spin binds the table the objects registered: a name the
     * announce cannot hold refuses here, before the ready line, instead of
     * failing the handshake with no detail. */
    node.spin_some();
    if (node.registration_error() != 0) {
        fprintf(stderr, "r2s_posix: cannot configure node '%s'\n", opts.node);
        return 2;
    }
    fprintf(stderr, "r2s_posix: ready node=%s rate=%u\n", opts.node, opts.rate);
    interval = (uint32_t)(1000u / opts.rate);
    last_pub = node.now();
    last_time = last_pub;
    for (;;) {
        uint32_t now;

        node.spin_some();
        now = node.now();
        if (node.connected() == 0) {
            /* Offline there are no samples, so the clock resyncs instead of
             * queueing a burst for the reconnect. */
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
            /* The cap shapes steady-state traffic only: the handshake above
             * ran uncapped, so its longer frames always fit. */
            r2s_posix_set_tx_cap(opts.tx_cap);
            cap_armed = 1u;
        }
        if (opts.log_lines != 0u && (uint32_t)(now - last_log) >= LOG_PERIOD_MS) {
            emit_log_lines();
            last_log = now;
        }
        if (opts.log_text != NULL && log_booted == 0u) {
            /* A boot line that fails to publish is retried until it sends.
             * It carries the assigned session token, so the host sees what
             * the firmware stored. */
            if (publish_log_text(log_pub, r2s_session_token()) != 0) {
                log_booted = 1u;
            }
        }
        if (opts.pub != NULL) {
            if (opts.flood != 0u) {
                /* Saturate the link: publish until the port refuses, one
                 * sample per attempt so the shed samples show as gaps in
                 * the received values. Bounded per pass, so a build that
                 * never refuses still returns to its tick. */
                uint16_t n;

                for (n = 0u; n < FLOOD_MAX_PER_PASS; n++) {
                    std_msgs::msg::Float32 msg;
                    r2s_status_t status;

                    msg.data = static_cast<float>(sent);
                    sent++;
                    status = battery_pub->publish(msg);
                    if (status == R2S_OK) {
                        continue;
                    }
                    if (status == R2S_ERR_TX_BUSY) {
                        busy++;
                    }
                    break;
                }
                if ((now - last_flood) >= FLOOD_REPORT_MS) {
                    fprintf(stderr, "r2s_posix: flood sent=%lu busy=%lu\n", static_cast<unsigned long>(sent),
                            static_cast<unsigned long>(busy));
                    last_flood = now;
                }
            } else {
                /* Every missed tick is still owed: one frame per missed interval
                 * keeps the count whole when the host steals the firmware tick. */
                while ((uint32_t)(now - last_pub) >= interval) {
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
            }
        } else {
            sleep_1ms();
        }
        if (opts.time_now != NULL && r2s_time_synced() != 0u) {
            /* One stamp per interval, no catch-up: a stalled link skips
             * samples the host would read as the past. */
            if ((uint32_t)(now - last_time) >= interval) {
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
