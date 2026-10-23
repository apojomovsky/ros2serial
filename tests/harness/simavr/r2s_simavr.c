/* simavr harness: an emulated AVR's UART0 bridged to a pty, for system tests.
   It links libsimavr, which only test-only code may do (AGENTS.md, Licensing boundaries). */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <simavr/avr_uart.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/sim_irq.h>

#define DEFAULT_FREQUENCY 16000000UL
#define DEFAULT_RUN_MS 10000UL

/* 1600 cycles is 100 us at 16 MHz: fast enough that an echo test sees no
   latency it cares about, rare enough that the syscall stays off the hot path. */
#define POLL_CYCLES 1600
#define INPUT_RING 4096
#define INPUT_CHUNK 256
#define PAINT_BYTE 0xa5

enum {
    EXIT_OK = 0,
    EXIT_USAGE = 2,
    EXIT_TIME_LIMIT = 3,
    EXIT_CPU_CRASHED = 4,
};

struct options {
    const char *mcu;
    const char *firmware;
    unsigned long frequency;
    unsigned long run_ms;
    int stack_paint;
    int pace;
};

struct harness {
    avr_t *avr;
    struct options options;
    int master_fd;
    int slave_fd;
    avr_irq_t *uart_input;
    int uart_xoff;
    unsigned char ring[INPUT_RING];
    size_t ring_head;
    size_t ring_tail;
    uint16_t paint_from;
    uint16_t paint_to;
    unsigned long long start_ms;
};

static struct harness harness;
static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void fail(const char *message)
{
    fprintf(stderr, "r2s_simavr: %s\n", message);
    exit(EXIT_USAGE);
}

static void fail_errno(const char *what)
{
    fprintf(stderr, "r2s_simavr: %s: %s\n", what, strerror(errno));
    exit(EXIT_USAGE);
}

static unsigned long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fail_errno("cannot read the clock");
    }
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

static unsigned long long simulated_ms(const struct harness *state)
{
    return state->avr->cycle * 1000ULL / state->avr->frequency;
}

static void uart_output(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    (void)param;
    unsigned char byte = (unsigned char)value;
    /* A client that stops reading fills the pty buffer; the byte is lost then,
       as it would be on a real link with no flow control towards the host. */
    if (write(harness.master_fd, &byte, 1) != 1) {
        return;
    }
}

static void flush_input(void)
{
    while (!harness.uart_xoff && harness.ring_head != harness.ring_tail) {
        avr_raise_irq(harness.uart_input, harness.ring[harness.ring_head]);
        harness.ring_head = (harness.ring_head + 1) % INPUT_RING;
    }
}

static void uart_xon(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    (void)value;
    (void)param;
    harness.uart_xoff = 0;
    flush_input();
}

static void uart_xoff(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    (void)value;
    (void)param;
    harness.uart_xoff = 1;
}

static void read_client(void)
{
    unsigned char chunk[INPUT_CHUNK];
    ssize_t count;
    while ((count = read(harness.master_fd, chunk, sizeof chunk)) > 0) {
        for (ssize_t i = 0; i < count; i++) {
            size_t next = (harness.ring_tail + 1) % INPUT_RING;
            if (next == harness.ring_head) {
                return; /* Full ring: the pty keeps the rest until the AVR drains it. */
            }
            harness.ring[harness.ring_tail] = chunk[i];
            harness.ring_tail = next;
        }
    }
    flush_input();
}

static void pace(struct harness *state)
{
    unsigned long long simulated = simulated_ms(state);
    unsigned long long wall = monotonic_ms() - state->start_ms;
    if (simulated <= wall) {
        return;
    }
    unsigned long long ahead = simulated - wall;
    struct timespec pause = {
        .tv_sec = (time_t)(ahead / 1000ULL),
        .tv_nsec = (long)((ahead % 1000ULL) * 1000000ULL),
    };
    nanosleep(&pause, NULL);
}

static int open_pty(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        fail_errno("cannot create a pty");
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        fail_errno("cannot prepare the pty");
    }
    const char *slave_path = ptsname(master);
    if (!slave_path) {
        fail_errno("cannot name the pty slave");
    }
    harness.slave_fd = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (harness.slave_fd < 0) {
        fail_errno("cannot open the pty slave");
    }
    struct termios settings;
    if (tcgetattr(harness.slave_fd, &settings) != 0) {
        fail_errno("cannot read the pty settings");
    }
    cfmakeraw(&settings);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(harness.slave_fd, TCSANOW, &settings) != 0) {
        fail_errno("cannot set the pty settings");
    }
    int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) != 0) {
        fail_errno("cannot unblock the pty");
    }
    /* Keeping the slave open means the master accepts writes before a client
       connects, so a byte the firmware sends during boot is not lost. */
    printf("r2s_simavr: pty=%s\n", slave_path);
    fflush(stdout);
    return master;
}

static uint16_t end_of_bss(const elf_firmware_t *firmware, const char *path)
{
    for (uint32_t i = 0; i < firmware->symbolcount; i++) {
        if (strcmp(firmware->symbol[i]->symbol, "_end") == 0) {
            /* AVR symbol addresses carry the 0x800000 SRAM window. */
            return (uint16_t)(firmware->symbol[i]->addr & 0xffff);
        }
    }
    fprintf(stderr, "r2s_simavr: %s has no _end symbol; link the firmware with its symbols\n", path);
    exit(EXIT_USAGE);
}

static unsigned stack_peak(const struct harness *state)
{
    if (!state->options.stack_paint) {
        return 0;
    }
    for (uint16_t address = state->paint_from; address <= state->paint_to; address++) {
        if (state->avr->data[address] != PAINT_BYTE) {
            return (unsigned)(state->paint_to - address + 1);
        }
    }
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: r2s_simavr --mcu <name> --firmware <elf> [--frequency <hz>] [--run-ms <ms>]\n"
                 "                  [--stack-paint] [--pace]\n");
}

static void parse_options(int argc, char **argv, struct options *options)
{
    options->frequency = DEFAULT_FREQUENCY;
    options->run_ms = DEFAULT_RUN_MS;
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        int has_value = i + 1 < argc;
        if (strcmp(option, "--help") == 0) {
            usage(stdout);
            exit(EXIT_OK);
        } else if (strcmp(option, "--stack-paint") == 0) {
            options->stack_paint = 1;
        } else if (strcmp(option, "--pace") == 0) {
            options->pace = 1;
        } else if (strcmp(option, "--mcu") == 0 && has_value) {
            options->mcu = argv[++i];
        } else if (strcmp(option, "--firmware") == 0 && has_value) {
            options->firmware = argv[++i];
        } else if (strcmp(option, "--frequency") == 0 && has_value) {
            options->frequency = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(option, "--run-ms") == 0 && has_value) {
            options->run_ms = strtoul(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "r2s_simavr: unknown or incomplete option %s\n", option);
            usage(stderr);
            exit(EXIT_USAGE);
        }
    }
    if (!options->mcu || !options->firmware) {
        usage(stderr);
        exit(EXIT_USAGE);
    }
    if (options->frequency == 0 || options->run_ms == 0) {
        fail("--frequency and --run-ms must be positive");
    }
}

int main(int argc, char **argv)
{
    parse_options(argc, argv, &harness.options);

    elf_firmware_t firmware;
    memset(&firmware, 0, sizeof firmware);
    if (elf_read_firmware(harness.options.firmware, &firmware) != 0) {
        fprintf(stderr, "r2s_simavr: cannot load %s\n", harness.options.firmware);
        return EXIT_USAGE;
    }
    harness.avr = avr_make_mcu_by_name(harness.options.mcu);
    if (!harness.avr) {
        fprintf(stderr, "r2s_simavr: unknown mcu %s\n", harness.options.mcu);
        return EXIT_USAGE;
    }
    harness.avr->frequency = (uint32_t)harness.options.frequency;
    avr_init(harness.avr);
    avr_load_firmware(harness.avr, &firmware);

    if (harness.options.stack_paint) {
        harness.paint_from = end_of_bss(&firmware, harness.options.firmware);
        harness.paint_to = harness.avr->ramend;
        if (harness.paint_from <= harness.avr->ioend || harness.paint_from > harness.paint_to) {
            fail("the _end symbol is not inside the AVR's SRAM");
        }
        memset(harness.avr->data + harness.paint_from, PAINT_BYTE,
               (size_t)(harness.paint_to - harness.paint_from + 1));
    }

    harness.master_fd = open_pty();

    /* The library sleeps 1 ms on every UART poll, which holds an idle firmware
       to real time; CI needs simulated time to run free, so the flag goes. */
    uint32_t uart_flags = 0;
    if (avr_ioctl(harness.avr, AVR_IOCTL_UART_SET_FLAGS('0'), &uart_flags) != 0) {
        fail("the mcu has no UART0");
    }
    uint32_t uart_irq = AVR_IOCTL_UART_GETIRQ('0');
    avr_irq_register_notify(avr_io_getirq(harness.avr, uart_irq, UART_IRQ_OUTPUT), uart_output, NULL);
    avr_irq_register_notify(avr_io_getirq(harness.avr, uart_irq, UART_IRQ_OUT_XON), uart_xon, NULL);
    avr_irq_register_notify(avr_io_getirq(harness.avr, uart_irq, UART_IRQ_OUT_XOFF), uart_xoff, NULL);
    harness.uart_input = avr_io_getirq(harness.avr, uart_irq, UART_IRQ_INPUT);

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    harness.start_ms = monotonic_ms();
    avr_cycle_count_t last_poll = 0;
    int code = EXIT_OK;
    while (!stop_requested) {
        if (harness.avr->cycle - last_poll >= POLL_CYCLES) {
            last_poll = harness.avr->cycle;
            read_client();
            if (monotonic_ms() - harness.start_ms >= harness.options.run_ms) {
                code = EXIT_TIME_LIMIT;
                break;
            }
            if (harness.options.pace) {
                pace(&harness);
            }
        }
        int state = avr_run(harness.avr);
        if (state == cpu_Done) {
            break;
        }
        if (state == cpu_Crashed) {
            fprintf(stderr, "r2s_simavr: the cpu crashed at pc 0x%x\n", harness.avr->pc);
            code = EXIT_CPU_CRASHED;
            break;
        }
    }

    unsigned long long wall = monotonic_ms() - harness.start_ms;
    printf("r2s_simavr: stop cycles=%llu simulated_ms=%llu wall_ms=%llu",
           (unsigned long long)harness.avr->cycle, simulated_ms(&harness), wall);
    if (harness.options.stack_paint) {
        printf(" stack_peak=%u", stack_peak(&harness));
    }
    printf("\n");
    fflush(stdout);
    avr_terminate(harness.avr);
    return code;
}
