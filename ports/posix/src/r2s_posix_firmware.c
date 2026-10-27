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
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

#include "r2s_posix_port.h"

/* Publish rates in Hz, from once a second to the loop's 1 ms granularity. */
#define RATE_MIN 1u
#define RATE_MAX 100u

/* Default topic names the command-line flags rename: rename_for and the
 * launch check key on these, so a generator rename edits one place. */
static const char *const kDefaultPub = "battery";
static const char *const kDefaultSub = "cmd_vel";
static const char *const kDefaultLogText = "log_text";

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
    if (strcmp(name, kDefaultPub) == 0) {
        *drop = (opts->pub == NULL);
        return opts->pub;
    }
    if (strcmp(name, kDefaultSub) == 0) {
        *drop = (opts->sub == NULL);
        return opts->sub;
    }
    if (strcmp(name, kDefaultLogText) == 0) {
        *drop = (opts->log_text == NULL);
        return opts->log_text;
    }
    *drop = 0u;
    return name;
}

/* Copies the generated topic table, swapping in the command-line names. */
static uint8_t fill_topics(const struct options *opts, r2s_topic_t *topics)
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

/* Scratch entry the limit probe below binds: overlong candidates are refused,
 * and the real r2s_session_configure() rebinds the table before use. Static
 * storage, so the pointer the core keeps stays valid until then. */
static r2s_topic_t s_probe;
static char s_probe_name[R2S_MAX_FRAME];

/* Whether the core gate binds this entry with a topic name of len x's: the
 * probe asks r2s_topics_configure_ram() itself instead of copying its sum. */
static uint8_t probe_fits(const r2s_topic_t *entry, uint32_t len)
{
    uint32_t i;

    if (len >= (uint32_t)R2S_MAX_FRAME) {
        return 0u;
    }
    s_probe = *entry;
    for (i = 0u; i < len; i++) {
        s_probe_name[i] = 'x';
    }
    s_probe_name[len] = '\0';
    s_probe.topic_name = s_probe_name;
    return (uint8_t)(r2s_topics_configure_ram(&s_probe, 1u) == R2S_OK);
}

/* Longest launch name for this entry the core gate still binds: a binary
 * search finds the exact edge, so the configured names stay the length
 * bound with no hidden headroom. */
static size_t entry_name_limit(const r2s_topic_t *entry)
{
    uint32_t lo = 0u;
    uint32_t hi = (uint32_t)R2S_MAX_FRAME;

    if (probe_fits(entry, 0u) == 0u) {
        return 0u;
    }
    while (lo + 1u < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;

        if (probe_fits(entry, mid) != 0u) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (size_t)lo;
}

/* A launch name that would not fit the announce entry is rejected here, with
 * its length and the limit, instead of failing configure with no detail. The
 * limit is probed from the generated table, so it tracks the generator. A
 * flag whose default name the table no longer carries is rejected too: the
 * rename would silently do nothing. */
static void check_renamed_name(const char *flag, const char *name, const char *default_name)
{
    uint8_t i;

    check_name(flag, name);
    for (i = 0u; i < R2S_TOPIC_COUNT; i++) {
        size_t len;
        size_t limit;

        if (strcmp(r2s_topics[i].topic_name, default_name) != 0) {
            continue;
        }
        len = strlen(name);
        limit = entry_name_limit(&r2s_topics[i]);
        if (len > limit) {
            fprintf(stderr, "r2s_posix: %s topic name '%s' too long: %lu chars, limit %lu for type '%s'\n",
                    flag, name, (unsigned long)len, (unsigned long)limit, r2s_topics[i].type_name);
            usage(stderr);
            exit(2);
        }
        return;
    }
    fprintf(stderr, "r2s_posix: %s names a topic the configuration does not carry: '%s'\n", flag,
            default_name);
    usage(stderr);
    exit(2);
}

static void parse_options(int argc, char **argv, struct options *opts)
{
    int i;

    opts->port = argv[1];
    opts->node = "r2s_posix";
    opts->pub = kDefaultPub;
    opts->sub = kDefaultSub;
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
        check_renamed_name("--pub", opts->pub, kDefaultPub);
    }
    if (opts->sub != NULL) {
        check_renamed_name("--sub", opts->sub, kDefaultSub);
    }
    if (opts->log_text != NULL) {
        check_renamed_name("--log-text", opts->log_text, kDefaultLogText);
    }
}

static uint32_t s_cmd_vel_count;

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
    s_cmd_vel_count++;
    fprintf(stderr, "r2s_posix: cmd_vel n=%lu\n", (unsigned long)s_cmd_vel_count);
}

/* One log_text line: the value as 8 hex digits. Fixed width fits the
 * handwritten String bound of 8 characters for every id. */
static r2s_status_t publish_log_text(uint32_t value)
{
    std_msgs__msg__String msg;
    uint8_t i;

    memset(&msg, 0, sizeof msg);
    for (i = 0u; i < 8u; i++) {
        int digit = (int)((value >> 28u) & 0xFu);

        msg.data[i] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value <<= 4u;
    }
    msg.data[8] = '\0';
    return r2s_publish_log_text(&msg);
}

static void sleep_1ms(void)
{
    const struct timespec req = {0, 1000000L};

    (void)nanosleep(&req, NULL);
}

int main(int argc, char **argv)
{
    struct options opts;
    r2s_topic_t topics[R2S_TOPIC_COUNT];
    uint8_t count;
    uint32_t interval;
    uint32_t last_pub;
    uint32_t boot_id;
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
    /* Prime the counter before opening the port, so a bad boot file fails
     * before any byte reaches the line. */
    boot_id = r2s_port_boot_id();
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
        if (r2s_connected() == 0u) {
            sleep_1ms();
            continue;
        }
        if (opts.log_text != NULL && log_booted == 0u) {
            /* A boot line that fails to publish is retried until it sends. */
            if (publish_log_text(boot_id) == R2S_OK) {
                log_booted = 1u;
            }
        }
        if (opts.pub != NULL && (uint32_t)(now - last_pub) >= interval) {
            std_msgs__msg__Float32 msg;

            msg.data = (float)sent;
            if (r2s_publish_battery(&msg) == R2S_OK) {
                sent++;
                last_pub = now;
                if (opts.log_text != NULL && sent % 10u == 0u) {
                    (void)publish_log_text(sent);
                }
            }
        }
        sleep_1ms();
    }
    return 0;
}
