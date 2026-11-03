/* error: the MCU side of the error report channel (spec/protocol.md, section
 * 7.5). It counts what the session, the framer and the publish wrappers
 * report, and sends one 5-byte frame per code on topic ID 5. */
#include "r2s/r2s_error.h"

#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"

/* The payload of a report ([ERR-1]): code, topic, count and field. */
#define ERROR_PAYLOAD_LEN 5u

/* A busy transmitter reports at most this often ([ERR-3]). */
#define BUSY_WINDOW_MS 1000u

static struct {
    uint16_t pending[7];   /* occurrences since the code's last report */
    uint8_t topic[7];      /* the topic of the latest occurrence */
    uint8_t field[7];      /* the field of the latest occurrence */
    uint8_t conn_reported; /* a code 5 report went out in this connection */
    uint32_t busy_last;    /* the tick of the last code 6 report */
    uint8_t seq;           /* the next report frame's sequence */
} error;

static uint32_t ms_to_ticks(uint32_t ms)
{
    /* On 16-bit ints (AVR) ms * R2S_TICK_HZ would overflow, so the product is
     * 32-bit: at most 1000 * 10^6, which fits. */
    return (ms * (uint32_t)R2S_TICK_HZ) / 1000u;
}

static uint8_t due(uint8_t code, uint32_t now)
{
    if (code == R2S_ERROR_TX_BUSY) {
        return (uint8_t)((uint32_t)(now - error.busy_last) >= ms_to_ticks(BUSY_WINDOW_MS));
    }
    if (code == R2S_ERROR_NOT_CONNECTED) {
        return (uint8_t)(error.conn_reported == 0u);
    }
    return 1u;
}

static void send_one(uint8_t code, uint32_t now)
{
    if (r2s_tx_begin(5u, error.seq, ERROR_PAYLOAD_LEN) != R2S_OK) {
        return;
    }
    /* The reservation above holds the whole frame, so these cannot fail. */
    (void)r2s_tx_byte(code);
    (void)r2s_tx_byte(error.topic[code]);
    (void)r2s_tx_byte((uint8_t)(error.pending[code] & 0xFFu));
    (void)r2s_tx_byte((uint8_t)((error.pending[code] >> 8u) & 0xFFu));
    (void)r2s_tx_byte(error.field[code]);
    if (r2s_tx_end() != R2S_OK) {
        return;
    }
    error.seq++;
    error.pending[code] = 0u;
    if (code == R2S_ERROR_NOT_CONNECTED) {
        error.conn_reported = 1u;
    }
    if (code == R2S_ERROR_TX_BUSY) {
        error.busy_last = now;
    }
}

void r2s_error_note(uint8_t code, uint8_t topic_id, uint8_t field, uint16_t count)
{
    if (code < 1u || code > 6u) {
        return;
    }
    /* A wrapping add is the [ERR-6] wrap: 65535 occurrences between two
     * reports read back as 65535, and the 65536th starts the count over. */
    error.pending[code] = (uint16_t)(error.pending[code] + count);
    error.topic[code] = topic_id;
    error.field[code] = field;
    r2s_error_flush();
}

void r2s_error_flush(void)
{
    uint32_t now = r2s_port_tick();
    uint8_t code;

    for (code = 1u; code <= 6u; code++) {
        if (error.pending[code] == 0u || due(code, now) == 0u) {
            continue;
        }
        send_one(code, now);
    }
}

void r2s_error_note_connected(void)
{
    error.seq = 0u;
    error.conn_reported = 0u;
    r2s_error_flush();
}

#ifdef R2S_TESTING
void r2s_error_test_reset(void)
{
    uint8_t code;

    for (code = 1u; code <= 6u; code++) {
        error.pending[code] = 0u;
        error.topic[code] = 0xFFu;
        error.field[code] = 0xFFu;
    }
    error.conn_reported = 0u;
    error.busy_last = 0u;
    error.seq = 0u;
}
#endif
