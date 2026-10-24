/* tx: payload bytes in, one frame out through the port (spec/protocol.md,
 * sections 2 to 4). */
#include "r2s/r2s_tx.h"

#include <stddef.h>

#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_rcobs.h"

/* What r2s_port_tx_free() reports when the port may block. */
#define TX_FREE_BLOCKING 0xFFFFu

static struct {
    uint8_t active;       /* a frame is open between begin and end */
    uint16_t payload_len; /* the payload bytes the frame declared */
    uint16_t fed;         /* the payload bytes handed over so far */
    uint16_t crc;         /* over topic_id, seq and the payload (CHK-3) */
    r2s_rcobs_encoder_t enc;
    uint8_t chunk[R2S_TX_CHUNK];
    uint8_t chunk_len;
} tx;

static void flush(void)
{
    r2s_port_write(tx.chunk, tx.chunk_len);
    tx.chunk_len = 0u;
}

static void emit(uint8_t byte)
{
    tx.chunk[tx.chunk_len] = byte;
    tx.chunk_len++;
    if (tx.chunk_len == R2S_TX_CHUNK) {
        flush();
    }
}

static void encode(uint8_t byte)
{
    uint8_t out[R2S_RCOBS_MAX_EMIT];
    uint8_t written = r2s_rcobs_encode_byte(&tx.enc, byte, out);

    emit(out[0]);
    if (written > 1u) {
        emit(out[1]);
    }
}

uint16_t r2s_tx_wire_bound(uint16_t payload_len)
{
    uint16_t decoded = (uint16_t)(payload_len + R2S_TX_OVERHEAD);

    return (uint16_t)(decoded + decoded / 254u + 1u + 1u);
}

r2s_status_t r2s_tx_begin(uint8_t topic_id, uint8_t seq, uint16_t payload_len)
{
    uint16_t max_payload = (uint16_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD);
    uint16_t bound;
    uint16_t free_bytes;

    if (tx.active != 0u || payload_len > max_payload) {
        return R2S_ERR_BOUNDS;
    }
    /* The whole frame is reserved before the first byte moves: a port that
     * cannot take all of it now gets nothing, so no frame is left half-sent
     * and the caller never blocks on a slow link. */
    bound = r2s_tx_wire_bound(payload_len);
    free_bytes = r2s_port_tx_free();
    if (free_bytes != TX_FREE_BLOCKING && free_bytes < bound) {
        return R2S_ERR_TX_BUSY;
    }
    tx.active = 1u;
    tx.payload_len = payload_len;
    tx.fed = 0u;
    tx.crc = R2S_CRC16_INIT;
    tx.chunk_len = 0u;
    r2s_rcobs_encoder_init(&tx.enc);
    tx.crc = r2s_crc16_update(tx.crc, topic_id);
    tx.crc = r2s_crc16_update(tx.crc, seq);
    encode(topic_id);
    encode(seq);
    return R2S_OK;
}

r2s_status_t r2s_tx_byte(uint8_t byte)
{
    if (tx.active == 0u || tx.fed == tx.payload_len) {
        return R2S_ERR_BOUNDS;
    }
    tx.crc = r2s_crc16_update(tx.crc, byte);
    encode(byte);
    tx.fed++;
    return R2S_OK;
}

r2s_status_t r2s_tx_end(void)
{
    uint8_t out[R2S_RCOBS_MAX_EMIT];

    if (tx.active == 0u) {
        return R2S_ERR_BOUNDS;
    }
    tx.active = 0u;
    if (tx.fed != tx.payload_len) {
        /* The frame cannot be finished without the bytes it declared, and
         * leaving it open would run the next frame into its bytes: the
         * delimiter cuts it off and the receiver drops it. */
        emit(0x00u);
        flush();
        return R2S_ERR_BOUNDS;
    }
    encode((uint8_t)(tx.crc & 0xFFu));
    encode((uint8_t)(tx.crc >> 8u));
    (void)r2s_rcobs_encode_end(&tx.enc, out);
    emit(out[0]);
    emit(0x00u);
    flush();
    return R2S_OK;
}

#ifdef R2S_TESTING
void r2s_tx_test_reset(void)
{
    tx.active = 0u;
    tx.payload_len = 0u;
    tx.fed = 0u;
    tx.crc = R2S_CRC16_INIT;
    tx.chunk_len = 0u;
    r2s_rcobs_encoder_init(&tx.enc);
}
#endif
