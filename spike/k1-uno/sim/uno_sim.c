// Run an ATmega328P firmware in simavr at real-time pace, with UART0 on a pty.
// Prints the pty path on stdout. SIGUSR1 resets the MCU (reset scenarios).
// Usage: uno_sim <firmware.elf>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/parts/uart_pty.h>

static volatile sig_atomic_t reset_requested;
static void on_usr1(int sig) { (void)sig; reset_requested = 1; }

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <firmware.elf>\n", argv[0]);
        return 2;
    }
    elf_firmware_t fw = {0};
    if (elf_read_firmware(argv[1], &fw) != 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    avr_t *avr = avr_make_mcu_by_name("atmega328p");
    if (!avr)
        return 1;
    avr_init(avr);
    avr_load_firmware(avr, &fw);
    avr->frequency = 16000000;

    static uart_pty_t pty;
    uart_pty_init(avr, &pty);
    uart_pty_connect(&pty, '0');
    printf("%s\n", pty.pty.slavename);
    fflush(stdout);
    signal(SIGUSR1, on_usr1);

    double t0 = wall_s();
    avr_cycle_count_t c0 = avr->cycle, next_check = avr->cycle + 16000;
    for (;;) {
        int state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed) {
            fprintf(stderr, "simavr stopped (state %d)\n", state);
            return 1;
        }
        if (reset_requested) {
            reset_requested = 0;
            avr_reset(avr);
            t0 = wall_s();
            c0 = avr->cycle;
            next_check = c0 + 16000;
        }
        if (avr->cycle >= next_check) {
            next_check = avr->cycle + 16000; /* every simulated millisecond */
            double sim = (double)(avr->cycle - c0) / avr->frequency;
            double ahead = sim - (wall_s() - t0);
            if (ahead > 0.001) {
                struct timespec ts = {0, (long)(ahead * 1e9)};
                nanosleep(&ts, NULL);
            }
        }
    }
}
