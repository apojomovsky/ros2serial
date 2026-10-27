/* The handwritten test firmware wiring: publish wrappers and dispatch for the
 * topics handwritten.c configures. Only the tests that drive the link need
 * the handler, so this file links only into them. */
#include "handwritten.h"

#include <stddef.h>

#include "r2s/r2s_framer.h"
#include "r2s/r2s_tx.h"

static geometry_msgs__msg__Twist r2s_handwritten_cmd_vel;

/* Publishes through the interpreter: measures the payload, reserves its wire
 * form, then streams it, so a failed publish writes nothing. The emit pass
 * cannot fail after the measure pass, so its status is not checked. */
static r2s_status_t publish(uint8_t id, uint8_t index, uint16_t type, const void *msg)
{
    r2s_codec_out_t out = {NULL, 0u, 0u, 0u};
    uint8_t narrow;
    uint8_t field = 0xFFu;
    uint8_t seq;
    int8_t rc;

    if (r2s_connected() == 0u) {
        return R2S_ERR_NOT_CONNECTED;
    }
    if (index == 0xFFu || r2s_session_topic_status(index) != 0u) {
        return R2S_ERR_TOPIC_REJECTED;
    }
    narrow = (uint8_t)(r2s_session_topic_flags(index) & 0x02u);
    rc = r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, type, msg, narrow, &out, &field);
    if (rc != R2S_OK) {
        return rc;
    }
    seq = r2s_session_next_tx_seq(index);
    rc = r2s_tx_begin(id, seq, out.len);
    if (rc != R2S_OK) {
        return rc;
    }
    out.to_tx = 1u;
    out.len = 0u;
    (void)r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, type, msg, narrow, &out, &field);
    (void)r2s_tx_end();
    return R2S_OK;
}

r2s_status_t r2s_publish_battery(const std_msgs__msg__Float32 *msg)
{
    return publish(R2S_HANDWRITTEN_BATTERY_ID, r2s_session_find_topic(R2S_HANDWRITTEN_BATTERY_ID),
                   R2S_HANDWRITTEN_FLOAT32, msg);
}

void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t index;
    uint8_t narrow;
    uint8_t field = 0xFFu;

    if (topic_id != R2S_HANDWRITTEN_CMD_VEL_ID) {
        r2s_framer_count_refused();
        return;
    }
    index = r2s_session_find_topic(topic_id);
    if (index == 0xFFu || r2s_session_topic_status(index) != 0u) {
        r2s_framer_count_refused();
        return;
    }
    narrow = (uint8_t)(r2s_session_topic_flags(index) & 0x02u);
    (void)r2s_session_note_rx_seq(index, seq);
    if (r2s_codec_decode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, payload, len, narrow,
                         &r2s_handwritten_cmd_vel, &field) != R2S_OK) {
        r2s_framer_count_refused();
        return;
    }
    r2s_on_cmd_vel(&r2s_handwritten_cmd_vel);
}
