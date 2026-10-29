/* The cmake example resyncs without bursts: a slightly late tick keeps the
 * phase, a stale deadline publishes once and resyncs, and the offline
 * branch resets the deadline instead of replaying it. The test compiles the
 * real main.c with its port functions renamed to dead stubs, so every
 * publish below runs the example's own loop step against queue fakes while
 * the tick is frozen. The renamed publish counts attempts, which is the
 * observable the decision constrains. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_rcobs.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

#define PROTOCOL_VERSION 2u
#define READY_TOKEN 0x2Au

void loop_once(void);
void TIMER0_COMPA_vect(void);
r2s_status_t cmake_test_count_battery(const std_msgs__msg__Float32 *msg);

static uint8_t s_rx[4096];
static size_t s_rx_head;
static size_t s_rx_tail;
/* The tick the test believes in: the firmware millisecond source starts at
 * zero and only the timer interrupt advances it, which the test calls. */
static uint32_t s_test_ms;
static uint32_t s_publishes;
static float s_values[16];
static size_t s_value_count;

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    uint16_t count = 0u;

    while (count < max && s_rx_head != s_rx_tail) {
        buf[count] = s_rx[s_rx_head];
        s_rx_head = (s_rx_head + 1u) % sizeof s_rx;
        count++;
    }
    return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}

uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

/* Moves the firmware clock forward: the timer interrupt never fires on
 * the host, so the test calls it once per millisecond. Time only moves
 * forward within a case, matching the firmware's own tick. */
static void set_tick(uint32_t ms)
{
    while (s_test_ms < ms) {
        TIMER0_COMPA_vect();
        s_test_ms++;
    }
}

r2s_status_t cmake_test_count_battery(const std_msgs__msg__Float32 *msg)
{
    s_publishes++;
    if (s_value_count < sizeof s_values / sizeof s_values[0]) {
        s_values[s_value_count] = msg->data;
        s_value_count++;
    }
    return R2S_OK;
}

static void push_bytes(const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0u; i < len; i++) {
        s_rx[s_rx_tail] = data[i];
        s_rx_tail = (s_rx_tail + 1u) % sizeof s_rx;
    }
}

/* One host frame on the wire, through the core's own encoder and CRC. */
static void encode_wire(uint8_t topic, uint8_t seq, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t raw[660];
    uint16_t raw_len;
    uint16_t crc;
    r2s_rcobs_encoder_t enc;
    uint16_t i;
    uint8_t produced[2];
    uint8_t tail;

    raw[0] = topic;
    raw[1] = seq;
    memcpy(&raw[2], payload, payload_len);
    raw_len = (uint16_t)(4u + payload_len);
    crc = r2s_crc16_update_buf(R2S_CRC16_INIT, raw, (uint16_t)(2u + payload_len));
    raw[2u + payload_len] = (uint8_t)(crc & 0xFFu);
    raw[3u + payload_len] = (uint8_t)(crc >> 8u);
    r2s_rcobs_encoder_init(&enc);
    for (i = 0u; i < raw_len; i++) {
        uint8_t count = r2s_rcobs_encode_byte(&enc, raw[i], produced);
        uint8_t k;

        for (k = 0u; k < count; k++) {
            push_bytes(&produced[k], 1u);
        }
    }
    tail = r2s_rcobs_encode_end(&enc, produced);
    push_bytes(produced, tail);
    {
        const uint8_t delimiter = 0x00u;

        push_bytes(&delimiter, 1u);
    }
}

static void send_frame(uint8_t topic, uint8_t seq, const uint8_t *payload, uint16_t payload_len)
{
    encode_wire(topic, seq, payload, payload_len);
    loop_once();
    loop_once();
}

/* The handshake the host runs: hello, a zero status per topic and ready.
 * The topic walk is skipped and the two ids come from declaration order,
 * which the connected assert below verifies. */
static uint8_t s_seq;

static int handshake(const char *node_name)
{
    const uint8_t hello_req[] = {0x01u, PROTOCOL_VERSION};
    const uint8_t battery_status[] = {0x05u, 16u, 0x00u};
    const uint8_t cmd_vel_status[] = {0x05u, 17u, 0x00u};
    const uint8_t ready[] = {0x06u, READY_TOKEN, 0x00u, 0x00u, 0x00u};

    (void)node_name;
    s_seq = 0u;
    send_frame(0u, s_seq++, hello_req, sizeof hello_req);
    send_frame(0u, s_seq++, battery_status, sizeof battery_status);
    send_frame(0u, s_seq++, cmd_vel_status, sizeof cmd_vel_status);
    send_frame(0u, s_seq++, ready, sizeof ready);
    CHECK(r2s_connected() != 0u);
    return 0;
}

/* Boots the firmware with the tick frozen: main is never called, the loop
 * step runs directly, so the statics start zeroed like on reset. */
static int boot_to_connected(void)
{
    set_tick(10000u);
    CHECK(handshake("hello_cmake") == 0);
    return 0;
}

/* A slightly late tick publishes once and keeps the phase: the next frame
 * stays on the old grid instead of drifting by the lateness. */
static int phase(void)
{
    CHECK(boot_to_connected() == 0);
    set_tick(10099u);
    loop_once();
    CHECK(s_publishes == 0u);
    set_tick(10100u);
    loop_once();
    CHECK(s_publishes == 1u);
    set_tick(10250u);
    loop_once();
    CHECK(s_publishes == 2u);
    set_tick(10299u);
    loop_once();
    CHECK(s_publishes == 2u);
    set_tick(10300u);
    loop_once();
    CHECK(s_publishes == 3u);
    CHECK(s_values[1] == s_values[0] + 1.0f);
    CHECK(s_values[2] == s_values[1] + 1.0f);
    return 0;
}

/* A stale deadline publishes once and resyncs to now, never a burst. */
static int stale(void)
{
    CHECK(boot_to_connected() == 0);
    set_tick(10100u);
    loop_once();
    CHECK(s_publishes == 1u);
    set_tick(10600u);
    loop_once();
    loop_once();
    loop_once();
    CHECK(s_publishes == 2u);
    set_tick(10699u);
    loop_once();
    CHECK(s_publishes == 2u);
    set_tick(10700u);
    loop_once();
    CHECK(s_publishes == 3u);
    return 0;
}

/* Going offline resets the deadline: the reconnect publishes nothing until
 * the next period instead of replaying the offline gap. */
static int offline_case(void)
{
    uint32_t reconnect_publishes;

    CHECK(boot_to_connected() == 0);
    set_tick(10100u);
    loop_once();
    CHECK(s_publishes == 1u);
    set_tick(25000u);
    loop_once();
    loop_once();
    CHECK(s_publishes == 1u);
    CHECK(r2s_connected() == 0u);
    CHECK(handshake("hello_cmake") == 0);
    reconnect_publishes = s_publishes;
    CHECK(reconnect_publishes == 1u);
    set_tick(25099u);
    loop_once();
    CHECK(s_publishes == 1u);
    set_tick(25100u);
    loop_once();
    CHECK(s_publishes == 2u);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: test_cmake_timing <phase|stale|offline>\n");
        return 2;
    }
    if (strcmp(argv[1], "phase") == 0) {
        return phase();
    }
    if (strcmp(argv[1], "stale") == 0) {
        return stale();
    }
    if (strcmp(argv[1], "offline") == 0) {
        return offline_case();
    }
    printf("unknown case '%s'\n", argv[1]);
    return 2;
}
