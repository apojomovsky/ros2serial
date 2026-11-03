/* Flood firmware for avr scenario 12: the same four topics the slice
 * announces, but battery publishes until the port refuses instead of at
 * 50 Hz. Each attempt consumes its sample number, so a refused publish
 * shows as a gap in the received values; the busy count rides log_text as
 * 8 hex digits every 2 s. The TX budget lives in the test-only port hook
 * (R2S_TEST_TX_CAP), armed once the link connects so the handshake runs
 * uncapped, exactly like the posix --tx-free flag. */

#define R2S_NODE_NAME "r2s_slice"
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel) \
    X(Publisher, geometry_msgs::msg::Twist, echo_vel_pub, "echo_vel") \
    X(Publisher, std_msgs::msg::String, log_text_pub, "log_text")

#include <ros2serial.h>

#ifndef R2S_TEST_TX_CAP
#error "the flood sketch needs R2S_TEST_TX_CAP from the test build"
#endif

/* Test-only port hook, declared here so the shipped port header stays
 * untouched: the symbol exists only in test builds of the glue. */
extern "C" void r2s_test_set_tx_cap(uint16_t cap);

/* One flood pass publishes at most this many samples before returning to
 * the tick, and reports its cumulative totals at most this often in ms. */
#define FLOOD_MAX_PER_PASS 64u
#define FLOOD_REPORT_MS 2000u

static uint32_t s_sent;
static uint32_t s_busy;
static uint32_t s_last_report;
static uint8_t s_cap_armed;

/* Best effort echo, kept so the handshake announces the same topics as
 * the slice; scenario 12 never drives it. */
void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    (void)r2s_publish_echo_vel_pub(&msg);
}

/* One log_text line: the value as 8 hex digits. Fixed width fits the
 * bundled String bound of 16 characters for every id. */
static r2s_status_t publish_log_text(uint32_t value)
{
    std_msgs::msg::String msg;
    uint8_t i;

    for (i = 0u; i < 8u; i++) {
        uint8_t digit = (uint8_t)((value >> 28u) & 0xFu);

        msg.data[i] = (char)(digit < 10u ? (char)('0' + digit) : (char)('a' + digit - 10u));
        value <<= 4u;
    }
    msg.data[8] = '\0';
    return r2s_publish_log_text_pub(&msg);
}

void setup(void)
{
    Serial.begin(115200);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}

void loop(void)
{
    uint8_t n;
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    if (r2s_connected() == 0u) {
        /* Offline the cap drops, so a reconnect handshakes uncapped; the
         * report clock resyncs instead of queueing a burst for it. */
        if (s_cap_armed != 0u) {
            s_cap_armed = 0u;
            r2s_test_set_tx_cap(0xFFFFu);
        }
        s_last_report = now;
        return;
    }
    if (s_cap_armed == 0u) {
        /* The cap shapes steady-state traffic only: the handshake above
         * ran uncapped, so its longer frames always fit. */
        r2s_test_set_tx_cap(R2S_TEST_TX_CAP);
        s_cap_armed = 1u;
    }
    /* Saturate the link: publish until the port refuses, one sample per
     * attempt so the shed samples show as gaps in the received values.
     * Bounded per pass, so a build that never refuses still returns to
     * its tick. */
    for (n = 0u; n < FLOOD_MAX_PER_PASS; n++) {
        std_msgs::msg::Float32 msg;
        r2s_status_t st;

        msg.data = (float)s_sent;
        s_sent++;
        st = r2s_publish_battery_pub(&msg);
        if (st == R2S_OK) {
            continue;
        }
        if (st == R2S_ERR_TX_BUSY) {
            s_busy++;
        }
        break;
    }
    if ((uint32_t)(now - s_last_report) >= FLOOD_REPORT_MS) {
        /* A refused report is shed like any frame; the next cadence
         * retries with a larger count, so no totals are lost. */
        if (publish_log_text(s_busy) == R2S_OK) {
            s_last_report = now;
        }
    }
}
