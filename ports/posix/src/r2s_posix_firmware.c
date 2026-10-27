/* Test firmware: the generated Float32, Twist and bounded-String topics on
 * a pty, with launch-time names, so the end-to-end tests drive the real core
 * as a process (docs/00-design.md, C10). */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

#include "r2s_posix_port.h"

/* Publish rates in Hz, from once a second to the loop's 1 ms granularity. */
#define RATE_MIN 1u
#define RATE_MAX 100u

struct options {
    const char *port;
    const char *node;
    const char *pub;
    const char *sub;
    const char *log_text;
    unsigned rate;
};

/* The name a generated topic launches under: its command-line option, or
 * NULL with drop set to drop it. An entry the list below does not know keeps
 * the name the generator emitted, so a wider configuration still configures. */
static const char *rename_for(const struct options *opts, const char *name, uint8_t *drop)
{
    if (strcmp(name, "battery") == 0) {
        *drop = (opts->pub == NULL);
        return opts->pub;
    }
    if (strcmp(name, "cmd_vel") == 0) {
        *drop = (opts->sub == NULL);
        return opts->sub;
    }
    if (strcmp(name, "log_text") == 0) {
        *drop = (opts->log_text == NULL);
        return opts->log_text;
    }
    *drop = 0u;
    return name;
}

/* Copies the generated topic table, swapping in the command-line names. */
static uint8_t fill_topics(const struct options *opts, r2s_session_topic_t *topics)
{
    uint8_t count = 0u;
    uint8_t i;

    for (i = 0u; i < R2S_TOPIC_COUNT; i++) {
        const char *rename;
        uint8_t drop;

        topics[count] = r2s_topics[i];
        rename = rename_for(opts, r2s_topics[i].topic_name, &drop);
        if (drop != 0u) {
            continue;
        }
        topics[count].topic_name = rename;
        count++;
    }
    return count;
}

static void usage(FILE *out)
{
    fprintf(out, "Usage: r2s_posix_firmware PORT [--node NAME] [--rate HZ]\n"
                 "           [--pub NAME | --no-pub] [--sub NAME | --no-sub]\n"
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

static void parse_options(int argc, char **argv, struct options *opts)
{
    int i;

    opts->port = argv[1];
    opts->node = "r2s_posix";
    opts->pub = "battery";
    opts->sub = "cmd_vel";
    opts->log_text = "log_text";
    opts->rate = 10u;
    for (i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--node") == 0) {
            opts->node = take_value(argc, argv, &i, arg);
        } else if (strcmp(arg, "--rate") == 0) {
            opts->rate = take_rate(take_value(argc, argv, &i, arg));
        } else if (strcmp(arg, "--pub") == 0) {
            opts->pub = take_value(argc, argv, &i, arg);
        } else if (strcmp(arg, "--sub") == 0) {
            opts->sub = take_value(argc, argv, &i, arg);
        } else if (strcmp(arg, "--log-text") == 0) {
            opts->log_text = take_value(argc, argv, &i, arg);
        } else if (strcmp(arg, "--no-pub") == 0) {
            opts->pub = NULL;
        } else if (strcmp(arg, "--no-sub") == 0) {
            opts->sub = NULL;
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
    if (opts->pub != NULL) {
        check_name("--pub", opts->pub);
    }
    if (opts->sub != NULL) {
        check_name("--sub", opts->sub);
    }
    if (opts->log_text != NULL) {
        check_name("--log-text", opts->log_text);
    }
}

static uint32_t s_cmd_vel_count;

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
    s_cmd_vel_count++;
    fprintf(stderr, "r2s_posix: cmd_vel n=%lu\n", (unsigned long)s_cmd_vel_count);
}

static void sleep_1ms(void)
{
    const struct timespec req = {0, 1000000L};

    (void)nanosleep(&req, NULL);
}

int main(int argc, char **argv)
{
    struct options opts;
    r2s_session_topic_t topics[R2S_TOPIC_COUNT];
    uint8_t count;
    uint32_t interval;
    uint32_t last_pub;
    uint32_t sent = 0u;

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
    /* Prime the counter before opening the port, so a bad boot file fails
     * before any byte reaches the line. */
    (void)r2s_port_boot_id();
    if (r2s_posix_open(opts.port) < 0) {
        fprintf(stderr, "r2s_posix: cannot open '%s': %s\n", opts.port, strerror(errno));
        return 1;
    }
    count = fill_topics(&opts, topics);
    if (r2s_session_configure(opts.node, topics, count) != R2S_OK) {
        fprintf(stderr, "r2s_posix: cannot configure node '%s'\n", opts.node);
        return 2;
    }
    r2s_init();
    fprintf(stderr, "r2s_posix: ready node=%s topics=%u rate=%u\n", opts.node, count, opts.rate);
    interval = (uint32_t)(1000u / opts.rate);
    last_pub = r2s_port_tick();
    for (;;) {
        uint32_t now;

        r2s_spin_some();
        now = r2s_port_tick();
        if (opts.pub != NULL && r2s_connected() != 0u && (uint32_t)(now - last_pub) >= interval) {
            std_msgs__msg__Float32 msg;

            msg.data = (float)sent;
            if (r2s_publish_battery(&msg) == R2S_OK) {
                sent++;
                last_pub = now;
            }
        }
        sleep_1ms();
    }
    return 0;
}
