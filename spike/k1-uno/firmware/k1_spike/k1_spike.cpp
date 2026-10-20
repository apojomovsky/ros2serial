// K1 spike: the v3 framing (rCOBS + CRC-16/MCRF4XX) and a minimal session on
// an Arduino Uno, handwritten to measure flash, RAM and link behavior before
// the real core exists. Throwaway: nothing here ships.
#include <Arduino.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>

#ifndef SPIKE_BAUD
#define SPIKE_BAUD 115200
#endif
#ifndef SPIKE_BATTERY_HZ
#define SPIKE_BATTERY_HZ 50
#endif

#define RX_CAP 64
#define HEARTBEAT_MS 500
#define LINK_TIMEOUT_MS 2000
#define HELLO_PERIOD_MS 1000
#define HELLO_REPLY_GAP_MS 100

enum { T_CONTROL = 0, T_TIME = 1, T_HEARTBEAT = 4, T_BATTERY = 16, T_CMD_VEL = 17, T_ECHO = 18 };
enum { OP_HELLO_REQ = 1, OP_HELLO = 2, OP_TOPIC_REQ = 3, OP_TOPIC = 4, OP_TOPIC_STATUS = 5, OP_READY = 6 };
enum { OP_PING = 1, OP_PONG = 2 };

struct Twist { float lx, ly, lz, ax, ay, az; };

static const char node_name[] PROGMEM = "k1_spike";
static const char ty_float32[] PROGMEM = "std_msgs/msg/Float32";
static const char ty_twist[] PROGMEM = "geometry_msgs/msg/Twist";
static const char nm_battery[] PROGMEM = "battery";
static const char nm_cmd_vel[] PROGMEM = "cmd_vel";
static const char nm_echo[] PROGMEM = "cmd_vel_echo";

struct TopicEntry { uint8_t id; uint8_t flags; const char *type; const char *name; };
// flags: bit0 MCU subscribes, bit1 narrow (forced: double is 32 bits here), bit2 keep_latest
static const TopicEntry topics[] PROGMEM = {
    {T_BATTERY, 0x02, ty_float32, nm_battery},
    {T_CMD_VEL, 0x07, ty_twist, nm_cmd_vel},
    {T_ECHO, 0x02, ty_twist, nm_echo},
};
#define TOPIC_COUNT (sizeof(topics) / sizeof(topics[0]))

static uint8_t rx[RX_CAP];
static uint8_t rx_len;
static uint8_t rx_overflow;
static uint32_t rx_frame_tick;
static uint8_t tx_seq[T_ECHO + 1];
static uint8_t rx_seq_next;
static uint8_t connected;
static uint32_t boot_id;
static uint32_t last_host_ms, last_hb_ms, last_hello_ms, last_battery_ms;
static uint16_t c_rx_frames, c_crc_errors, c_rx_overflows, c_dropped, c_tx_busy;
static float battery_count;

static uint16_t crc_step(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (uint8_t i = 0; i < 8; i++)
        crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    return crc;
}

// Streaming rCOBS encoder: the code byte follows its block, so no TX buffer.
static uint8_t enc_run;
static uint16_t enc_crc;
static uint8_t chunk[8];
static uint8_t chunk_len;

static void raw(uint8_t b)
{
    chunk[chunk_len++] = b;
    if (chunk_len == sizeof(chunk)) {
        Serial.write(chunk, chunk_len);
        chunk_len = 0;
    }
}

static void rcobs_push(uint8_t b)
{
    if (b == 0) {
        raw(enc_run);
        enc_run = 1;
    } else {
        raw(b);
        if (++enc_run == 0xFF) {
            raw(0xFF);
            enc_run = 1;
        }
    }
}

static void put(uint8_t b)
{
    enc_crc = crc_step(enc_crc, b);
    rcobs_push(b);
}

static void put_u16(uint16_t v) { put((uint8_t)v); put((uint8_t)(v >> 8)); }
static void put_u32(uint32_t v) { put_u16((uint16_t)v); put_u16((uint16_t)(v >> 16)); }
static void put_f32(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    put_u32(c.u);
}

static void put_fstr(const char *p)
{
    uint8_t n = (uint8_t)strlen_P(p);
    put(n);
    while (n--)
        put(pgm_read_byte(p++));
}

static uint16_t frame_bound(uint16_t payload) { uint16_t n = payload + 4; return n + n / 254 + 2; }

// Returns 0 without writing anything when a user frame does not fit the TX buffer.
static uint8_t frame_begin(uint8_t topic, uint16_t payload_len)
{
    if (topic >= 16 && (uint16_t)Serial.availableForWrite() < frame_bound(payload_len)) {
        c_tx_busy++;
        return 0;
    }
    enc_run = 1;
    enc_crc = 0xFFFF;
    chunk_len = 0;
    put(topic);
    put(tx_seq[topic]++);
    return 1;
}

static void frame_end(void)
{
    uint16_t crc = enc_crc;
    rcobs_push((uint8_t)crc);
    rcobs_push((uint8_t)(crc >> 8));
    raw(enc_run);
    raw(0);
    if (chunk_len)
        Serial.write(chunk, chunk_len);
}

static void send_hello(void)
{
    frame_begin(T_CONTROL, 0);
    put(OP_HELLO);
    put(1);           // protocol version
    put(0);           // full handshake
    put_u32(boot_id);
    put_u16(RX_CAP);  // max frame
    put_u16(RX_CAP);  // rx capacity
    put_u32(1000000); // tick_hz: micros()
    put(0x11);        // features: time, double is 32 bits
    put(TOPIC_COUNT);
    put_fstr(node_name);
    frame_end();
    last_hello_ms = millis();
}

static void send_topic(uint8_t idx)
{
    if (idx >= TOPIC_COUNT)
        return;
    TopicEntry e;
    memcpy_P(&e, &topics[idx], sizeof(e));
    frame_begin(T_CONTROL, 0);
    put(OP_TOPIC);
    put(idx);
    put(e.id);
    put(e.flags);
    for (uint8_t i = 0; i < 8; i++)
        put(0); // RIHS01 prefix: placeholder in the spike
    put_fstr(e.type);
    put_fstr(e.name);
    frame_end();
}

static void send_heartbeat(void)
{
    frame_begin(T_HEARTBEAT, 0);
    put_u16(c_rx_frames);
    put_u16(c_crc_errors);
    put_u16(c_rx_overflows);
    put_u16(c_dropped);
    put_u16(c_tx_busy); // spike extra: not in the v3 heartbeat
    frame_end();
}

static void publish_twist(uint8_t topic, const Twist &t)
{
    if (!frame_begin(topic, 24))
        return;
    put_f32(t.lx); put_f32(t.ly); put_f32(t.lz);
    put_f32(t.ax); put_f32(t.ay); put_f32(t.az);
    frame_end();
}

static float get_f32(const uint8_t *p)
{
    union { float f; uint32_t u; } c;
    c.u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return c.f;
}

// Backward in-place decode; the result ends at buf + len. Returns its start, or -1.
static int16_t rcobs_decode(uint8_t *buf, uint8_t len)
{
    uint8_t i = len, w = len, last = 1;
    while (i > 0) {
        uint8_t c = buf[i - 1];
        if (c == 0 || c > i)
            return -1;
        if (!last && c != 0xFF)
            buf[--w] = 0;
        for (uint8_t k = 1; k < c; k++)
            buf[--w] = buf[i - 1 - k];
        i -= c;
        last = 0;
    }
    return w;
}

static void on_frame(const uint8_t *f, uint8_t n)
{
    uint8_t topic = f[0], seq = f[1];
    const uint8_t *p = f + 2;
    uint8_t plen = n - 4;
    uint32_t now = millis();
    last_host_ms = now;

    if (topic == T_CONTROL && plen >= 1) {
        switch (p[0]) {
        case OP_HELLO_REQ: connected = 0; send_hello(); return;
        case OP_TOPIC_REQ: if (plen >= 2) send_topic(p[1]); return;
        case OP_TOPIC_STATUS: return;
        case OP_READY:
            connected = 1;
            memset(tx_seq, 0, sizeof(tx_seq));
            rx_seq_next = 0;
            return;
        }
        c_dropped++;
        return;
    }
    if (!connected) {
        if (now - last_hello_ms >= HELLO_REPLY_GAP_MS)
            send_hello();
        return;
    }
    if (topic == T_TIME && plen >= 2 && p[0] == OP_PING) {
        frame_begin(T_TIME, 0);
        put(OP_PONG);
        put(p[1]);
        put_u32(rx_frame_tick);
        frame_end();
    } else if (topic == T_HEARTBEAT) {
        // nothing beyond refreshing the link
    } else if (topic == T_CMD_VEL && plen == 24) {
        if (seq != rx_seq_next)
            c_dropped++;
        rx_seq_next = seq + 1;
        Twist t = {get_f32(p), get_f32(p + 4), get_f32(p + 8), get_f32(p + 12), get_f32(p + 16), get_f32(p + 20)};
        publish_twist(T_ECHO, t);
    } else {
        c_dropped++;
    }
}

static void rx_byte(uint8_t b)
{
    if (b != 0) {
        if (rx_len < RX_CAP)
            rx[rx_len++] = b;
        else
            rx_overflow = 1;
        return;
    }
    rx_frame_tick = micros();
    uint8_t len = rx_len;
    rx_len = 0;
    if (rx_overflow) {
        rx_overflow = 0;
        c_rx_overflows++;
        return;
    }
    if (len == 0)
        return;
    int16_t start = rcobs_decode(rx, len);
    if (start < 0 || len - start < 4) {
        c_dropped++;
        return;
    }
    uint16_t crc = 0xFFFF;
    for (uint8_t i = (uint8_t)start; i < len; i++)
        crc = crc_step(crc, rx[i]);
    if (crc != 0) {
        c_crc_errors++;
        return;
    }
    c_rx_frames++;
    on_frame(rx + start, (uint8_t)(len - start));
}

void setup()
{
    boot_id = eeprom_read_dword((uint32_t *)0) + 1;
    eeprom_update_dword((uint32_t *)0, boot_id);
    Serial.begin(SPIKE_BAUD);
    send_hello();
}

void loop()
{
    int c;
    while ((c = Serial.read()) >= 0)
        rx_byte((uint8_t)c);

    uint32_t now = millis();
    if (connected && now - last_host_ms > LINK_TIMEOUT_MS)
        connected = 0;
    if (!connected) {
        if (now - last_hello_ms >= HELLO_PERIOD_MS && now - last_host_ms >= HELLO_PERIOD_MS)
            send_hello();
        return;
    }
    if (now - last_hb_ms >= HEARTBEAT_MS) {
        last_hb_ms = now;
        send_heartbeat();
    }
    if (now - last_battery_ms >= 1000 / SPIKE_BATTERY_HZ) {
        last_battery_ms = now;
        // A running count, not a voltage, so the host can count losses exactly.
        if (frame_begin(T_BATTERY, 4)) {
            put_f32(battery_count);
            frame_end();
            battery_count += 1.0f;
        }
    }
}
