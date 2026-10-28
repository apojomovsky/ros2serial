/* Runtime posix firmware: the same Float32, Twist and String topics as the
 * generated C firmware, registered as r2s::Node objects while the firmware
 * runs, so the end-to-end suite drives the runtime registration path as a
 * process on a pty. The command line mirrors the C firmware's, and the
 * topics register in the same order, so a dropped topic compacts the IDs
 * exactly the way the generated table does. */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ros2serial.h"

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

struct options {
    const char *port;
    const char *node;
    const char *pub;
    const char *sub;
    const char *echo;
    const char *log_text;
    unsigned rate;
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

static void usage(FILE *out)
{
    fprintf(out, "Usage: r2s_posix_runtime_firmware PORT [--node NAME] [--rate HZ]\n"
                 "           [--pub NAME | --no-pub] [--sub NAME | --no-sub]\n"
                 "           [--echo NAME | --no-echo]\n"
                 "           [--log-text NAME | --no-log-text]\n");
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
    opts->rate = 10u;
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
        } else if (strcmp(arg, "--no-pub") == 0) {
            opts->pub = NULL;
        } else if (strcmp(arg, "--no-sub") == 0) {
            opts->sub = NULL;
        } else if (strcmp(arg, "--no-echo") == 0) {
            opts->echo = NULL;
        } else if (strcmp(arg, "--no-log-text") == 0) {
            opts->log_text = NULL;
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
    uint32_t interval;
    uint32_t last_pub;
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
    if (r2s_posix_open(opts.port) < 0) {
        fprintf(stderr, "r2s_posix: cannot open '%s': %s\n", opts.port, strerror(errno));
        return 1;
    }
    /* Function-local objects, so the command line is parsed before anything
     * registers: a dropped topic is never constructed, and the IDs compact
     * in configuration order like the generated table. */
    static r2s::Node node(opts.node);
    r2s::Publisher<std_msgs::msg::Float32> *battery_pub = NULL;
    r2s::Publisher<std_msgs::msg::String> *log_pub = NULL;
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
    for (;;) {
        uint32_t now;

        node.spin_some();
        now = node.now();
        if (node.connected() == 0) {
            sleep_1ms();
            continue;
        }
        if (opts.log_text != NULL && log_booted == 0u) {
            /* A boot line that fails to publish is retried until it sends.
             * It carries the assigned session token, so the host sees what
             * the firmware stored. */
            if (publish_log_text(log_pub, r2s_session_token()) != 0) {
                log_booted = 1u;
            }
        }
        if (opts.pub != NULL && (uint32_t)(now - last_pub) >= interval) {
            std_msgs::msg::Float32 msg;

            msg.data = (float)sent;
            if (battery_pub->publish(msg) == R2S_OK) {
                sent++;
                last_pub = now;
            }
        } else {
            sleep_1ms();
        }
    }
    return 0;
}
