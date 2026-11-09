/* Port tick checks: monotonic, live, at 1000 Hz and pinned to CLOCK_MONOTONIC.
 * The rate check reads the same clock the port uses, so it cannot tell which
 * clock that is. The link-time wrap below sees every clock_gettime call the
 * port makes and fails the test unless each one names CLOCK_MONOTONIC, so a
 * port moved to CLOCK_REALTIME fails even though its rate still matches. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "r2s/r2s_port.h"
#include "r2s_posix_port.h"

/* Counts of the clock IDs the port requested, through the link-time wrap
 * (LINKER:--wrap=clock_gettime in CMakeLists). The helpers below call the
 * real function directly, so only the port's requests land here. */
static unsigned long s_monotonic_calls;
static unsigned long s_other_calls;

int __real_clock_gettime(clockid_t clk_id, struct timespec *tp);
int __wrap_clock_gettime(clockid_t clk_id, struct timespec *tp);

int __wrap_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    /* A test-controlled offset in milliseconds: the skew check below moves
     * the clock by exact amounts instead of sleeping through real time. */
  extern long r2s_test_clock_offset_ms;
  long carry;

  if (clk_id == CLOCK_MONOTONIC) {
    s_monotonic_calls++;
  } else {
    s_other_calls++;
  }
  if (__real_clock_gettime(clk_id, tp) != 0) {
    return -1;
  }
  if (r2s_test_clock_offset_ms == 0L) {
    return 0;
  }
  tp->tv_sec += r2s_test_clock_offset_ms / 1000L;
  carry = r2s_test_clock_offset_ms % 1000L;
  tp->tv_nsec += carry * 1000000L;
  if (tp->tv_nsec >= 1000000000L) {
    tp->tv_sec += 1;
    tp->tv_nsec -= 1000000000L;
  }
  if (tp->tv_nsec < 0L) {
    tp->tv_sec -= 1;
    tp->tv_nsec += 1000000000L;
  }
  return 0;
}

/* The wrap's offset, moved by exact amounts so the skew check below never
 * sleeps and never depends on scheduling. */
long r2s_test_clock_offset_ms;

/* A skewed tick still advances monotonically, scaled by the skew: a fake
 * second reads 1010 ticks at +10000 ppm and 980 at -20000 ppm, each within
 * one tick of real time passing between the two reads. */
static int check_skew(void)
{
  static const struct
  {
    int32_t ppm;
    uint32_t want;
  } cases[] = {{10000, 1010u}, {-20000, 980u}, {0, 1000u}};
  size_t i;

  for (i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
    uint32_t before;
    uint32_t after;
    uint32_t span;

    r2s_posix_set_tick_skew_ppm(cases[i].ppm);
    before = r2s_port_tick();
    r2s_test_clock_offset_ms += 1000L;
    after = r2s_port_tick();
    span = after - before;
    if (span < cases[i].want - 1u || span > cases[i].want + 2u) {
      fprintf(stderr, "skew %ld ppm moved %lu ticks in one fake second, want %lu\n",
        (long)cases[i].ppm,
        (unsigned long)span, (unsigned long)cases[i].want);
      return 1;
    }
  }
  return 0;
}

#define SAMPLES 100000u

static void sleep_ms(long ms)
{
  const struct timespec req = {ms / 1000L, (ms % 1000L) * 1000000L};

  (void)nanosleep(&req, NULL);
}

/* Wall time from CLOCK_MONOTONIC through the real call, so these reads never
 * touch the wrap counters: only the port's requests land there. */
static uint64_t wall_ms(void)
{
  struct timespec now;

  (void)__real_clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000L);
}

static int check_monotonic(void)
{
  uint32_t prev = r2s_port_tick();
  uint32_t i;

  for (i = 0u; i < SAMPLES; i++) {
    uint32_t now = r2s_port_tick();

    if (now < prev) {
      fprintf(stderr, "tick went back from %lu to %lu\n", (unsigned long)prev, (unsigned long)now);
      return 1;
    }
    prev = now;
  }
  return 0;
}

/* Fails unless the port read CLOCK_MONOTONIC for every tick and nothing else.
 * A port moved to another clock keeps the 1:1 rate above, so only this check
 * pins the source. */
static int check_clock_source(unsigned long mono_base, unsigned long other_base)
{
  if (s_monotonic_calls == mono_base) {
    fprintf(stderr, "tick never read CLOCK_MONOTONIC\n");
    return 1;
  }
  if (s_other_calls != other_base) {
    fprintf(stderr, "tick read a clock other than CLOCK_MONOTONIC\n");
    return 1;
  }
  return 0;
}

int main(void)
{
  uint32_t before;
  uint32_t after;
  uint64_t wall_before;
  uint64_t wall_after;
  uint64_t elapsed;
  uint64_t span;
  unsigned long mono_base = s_monotonic_calls;
  unsigned long other_base = s_other_calls;
  if (check_monotonic() != 0) {
    return 1;
  }
  before = r2s_port_tick();
  sleep_ms(5L);
  after = r2s_port_tick();
  if (after <= before) {
    fprintf(stderr, "tick did not advance in 5 ms\n");
    return 1;
  }
  wall_before = wall_ms();
  before = r2s_port_tick();
  sleep_ms(200L);
  wall_after = wall_ms();
  after = r2s_port_tick();
  elapsed = wall_after - wall_before;
  span = (uint64_t)(after - before);
  if (elapsed < 100u) {
    fprintf(stderr, "wall clock did not advance\n");
    return 1;
  }
  if (span * 5u < elapsed * 4u || span * 5u > elapsed * 6u) {
    fprintf(stderr, "tick moved %lu in %lu ms, want a 1:1 ratio\n", (unsigned long)span,
      (unsigned long)elapsed);
    return 1;
  }
  if (check_skew() != 0) {
    return 1;
  }
  if (check_clock_source(mono_base, other_base) != 0) {
    return 1;
  }
  printf("tick ok: monotonic, live, 1000 Hz, CLOCK_MONOTONIC\n");
  return 0;
}
