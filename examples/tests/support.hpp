/* Host harness for the example timing tests: a tap stream, the wire codec
 * through the core's public rCOBS and CRC calls, and the handshake driver.
 * The tests compile the real sketches against the Arduino stubs, so every
 * publish below runs the example's own loop. */
#ifndef R2S_EXAMPLE_TEST_SUPPORT_HPP
#define R2S_EXAMPLE_TEST_SUPPORT_HPP

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "Arduino.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_rcobs.h"

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

#define R2S_EXAMPLE_PROTOCOL_VERSION 2u
#define R2S_EXAMPLE_READY_TOKEN 0x2Au

/* A stream the test drives: pushed bytes feed the firmware, written bytes
 * stay in the log. Attaching it after setup replaces the default Serial
 * without touching the sketch. */
class TapStream : public Stream
{
  public:
    void begin(unsigned long baud)
    {
        (void)baud;
    }

    void push(const uint8_t *data, size_t len)
    {
        m_rx.insert(m_rx.end(), data, data + len);
    }

    size_t write(const uint8_t *buffer, size_t size) override
    {
        m_tx.insert(m_tx.end(), buffer, buffer + size);
        return size;
    }

    int availableForWrite(void) override
    {
        return 64;
    }

    int available(void) override
    {
        return (int)m_rx.size();
    }

    int read(void) override
    {
        uint8_t next;

        if (m_rx.empty()) {
            return -1;
        }
        next = m_rx.front();
        m_rx.erase(m_rx.begin());
        return (int)next;
    }

    const std::vector<uint8_t> &tx(void) const
    {
        return m_tx;
    }

  private:
    std::vector<uint8_t> m_rx;
    std::vector<uint8_t> m_tx;
};

/* One decoded frame: its topic, seq and payload without the CRC. */
struct TestFrame {
    uint8_t topic;
    uint8_t seq;
    std::vector<uint8_t> payload;
};

/* The wire form of one frame: topic, seq, payload, CRC-16/MCRF4XX and the
 * rCOBS encoding with its delimiter, through the core's own calls. */
static inline void encode_wire(uint8_t topic, uint8_t seq, const uint8_t *payload, uint16_t payload_len,
                               std::vector<uint8_t> &out)
{
    uint8_t raw[660];
    uint16_t raw_len;
    uint16_t crc;
    r2s_rcobs_encoder_t enc;
    uint16_t i;
    uint8_t produced[2];

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

        out.insert(out.end(), produced, produced + count);
    }
    {
        uint8_t count = r2s_rcobs_encode_end(&enc, produced);

        out.insert(out.end(), produced, produced + count);
    }
    out.push_back(0x00u);
}

/* Decodes every complete frame from tx starting at offset, and returns the
 * new offset. A frame with a bad CRC fails the test, so a miscount never
 * hides behind corruption. */
static inline int decode_frames(const std::vector<uint8_t> &tx, size_t &offset,
                                std::vector<TestFrame> &frames)
{
    while (offset < tx.size()) {
        size_t end = offset;
        uint8_t work[700];
        uint8_t *decoded = NULL;
        uint16_t decoded_len = 0u;
        uint16_t crc;
        TestFrame frame;
        size_t span;

        while (end < tx.size() && tx[end] != 0x00u) {
            end++;
        }
        if (end >= tx.size()) {
            break;
        }
        span = end - offset;
        CHECK(span < sizeof work);
        memcpy(work, &tx[offset], span);
        CHECK(r2s_rcobs_decode(work, (uint16_t)span, &decoded, &decoded_len) == R2S_RCOBS_OK);
        CHECK(decoded_len >= 4u);
        crc = r2s_crc16_update_buf(R2S_CRC16_INIT, decoded, (uint16_t)(decoded_len - 2u));
        CHECK(decoded[decoded_len - 2u] == (uint8_t)(crc & 0xFFu));
        CHECK(decoded[decoded_len - 1u] == (uint8_t)(crc >> 8u));
        frame.topic = decoded[0];
        frame.seq = decoded[1];
        frame.payload.assign(&decoded[2], &decoded[decoded_len - 2u]);
        frames.push_back(frame);
        offset = end + 1u;
    }
    return 0;
}

/* One LEB128 value: the varint at at, advancing past it. */
static inline bool read_varint(const std::vector<uint8_t> &payload, size_t &at, uint32_t &value)
{
    uint32_t result = 0u;
    unsigned shift = 0u;

    while (at < payload.size()) {
        uint8_t next = payload[at++];

        result |= (uint32_t)((uint32_t)(next & 0x7Fu) << shift);
        if (next < 0x80u) {
            value = result;
            return true;
        }
        shift += 7u;
        if (shift >= 32u) {
            return false;
        }
    }
    return false;
}

/* One varint-length-prefixed string, advancing past it. */
static inline bool read_string(const std::vector<uint8_t> &payload, size_t &at, std::string &out)
{
    uint32_t len;

    if (!read_varint(payload, at, len)) {
        return false;
    }
    if ((size_t)len > payload.size() - at) {
        return false;
    }
    out.assign((const char *)&payload[at], len);
    at += len;
    return true;
}

/* Feeds one host frame to the firmware and spins until its control reply
 * arrives or the loop budget runs out. The tick stays frozen, so no
 * heartbeat or hello repeat interleaves with the solicited reply. */
static inline int transact(TapStream &tap, void (*spin)(void), uint8_t topic, uint8_t seq,
                           const uint8_t *payload, uint16_t payload_len, uint8_t want_op,
                           std::vector<uint8_t> &reply, size_t &offset)
{
    std::vector<uint8_t> wire;
    unsigned int spins;

    encode_wire(topic, seq, payload, payload_len, wire);
    tap.push(wire.data(), wire.size());
    for (spins = 0u; spins < 10u; spins++) {
        std::vector<TestFrame> frames;
        size_t i;

        spin();
        CHECK(decode_frames(tap.tx(), offset, frames) == 0);
        for (i = 0u; i < frames.size(); i++) {
            if (frames[i].topic == 0u && !frames[i].payload.empty() && frames[i].payload[0] == want_op) {
                reply = frames[i].payload;
                return 0;
            }
        }
    }
    printf("FAIL %s:%d: no control reply %02x\n", __FILE__, __LINE__, want_op);
    return 1;
}

/* Sends one host frame without waiting for a reply. Statuses draw none,
 * so the handshake sends them this way. */
static inline void send_frame(TapStream &tap, void (*spin)(void), uint8_t topic, uint8_t seq,
                              const uint8_t *payload, uint16_t payload_len)
{
    std::vector<uint8_t> wire;

    encode_wire(topic, seq, payload, payload_len, wire);
    tap.push(wire.data(), wire.size());
    spin();
    spin();
}

/* The handshake the host runs: hello, the topic walk, a zero status per
 * topic and ready. Returns the battery topic id, found by name, so the
 * tests never hard-code an id the table could renumber. */
static inline int handshake(TapStream &tap, void (*spin)(void), const char *node_name, uint8_t &battery_id,
                            size_t &offset)
{
    const uint8_t hello_req[] = {0x01u, R2S_EXAMPLE_PROTOCOL_VERSION};
    const uint8_t ready[] = {0x06u, R2S_EXAMPLE_READY_TOKEN, 0x00u, 0x00u, 0x00u};
    std::vector<uint8_t> reply;
    uint8_t count;
    uint8_t seq = 0u;
    uint8_t index;
    std::string name;

    CHECK(transact(tap, spin, 0u, seq++, hello_req, sizeof hello_req, 0x02u, reply, offset) == 0);
    CHECK(reply.size() >= 17u);
    CHECK(reply[1] == R2S_EXAMPLE_PROTOCOL_VERSION);
    count = reply[16];
    CHECK(count > 0u);
    {
        size_t at = 17u;

        CHECK(read_string(reply, at, name));
        CHECK(name == node_name);
    }
    battery_id = 0xFFu;
    for (index = 0u; index < count; index++) {
        const uint8_t topic_req[] = {0x03u, index};
        std::string type;
        size_t at = 12u;

        CHECK(transact(tap, spin, 0u, seq++, topic_req, sizeof topic_req, 0x04u, reply, offset) == 0);
        CHECK(reply.size() >= 13u);
        CHECK(reply[1] == index);
        CHECK(read_string(reply, at, type));
        CHECK(read_string(reply, at, name));
        if (name == "battery") {
            battery_id = reply[2];
        }
        {
            const uint8_t status[] = {0x05u, reply[2], 0x00u};

            send_frame(tap, spin, 0u, seq++, status, sizeof status);
        }
    }
    CHECK(battery_id != 0xFFu);
    send_frame(tap, spin, 0u, seq++, ready, sizeof ready);
    {
        /* The ready spins may publish when the deadline is stale; the tests
         * assert on what follows, so the handshake consumes everything. */
        std::vector<TestFrame> drained;

        CHECK(decode_frames(tap.tx(), offset, drained) == 0);
    }
    return 0;
}

/* Collects the battery values published since offset. Each value carries
 * the tick that produced it, so the tests assert spacing on the firmware
 * clock instead of the wall clock. */
struct BatterySample {
    uint32_t tick;
    float value;
};

static inline int collect_battery(TapStream &tap, size_t &offset, uint8_t battery_id, uint32_t tick,
                                  std::vector<BatterySample> &samples)
{
    std::vector<TestFrame> frames;
    size_t i;

    CHECK(decode_frames(tap.tx(), offset, frames) == 0);
    for (i = 0u; i < frames.size(); i++) {
        BatterySample sample;

        if (frames[i].topic != battery_id) {
            continue;
        }
        CHECK(frames[i].payload.size() == 4u);
        memcpy(&sample.value, frames[i].payload.data(), 4u);
        sample.tick = tick;
        samples.push_back(sample);
    }
    return 0;
}

#endif
