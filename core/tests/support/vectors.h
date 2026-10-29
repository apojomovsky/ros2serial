/* Records of conformance vector cases, filled by the runners that
 * tools/unity_runner.py generates. Each byte field comes as the bytes, their
 * length and the vector's own hex text, and every record names its file,
 * case and spec IDs for failure messages. */
#ifndef R2S_TEST_VECTORS_H
#define R2S_TEST_VECTORS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *where;
    const char *covers;
    const uint8_t *data;
    uint16_t data_len;
    const char *data_hex; /* NULL when the vector gives the data as text */
    const char *text;     /* NULL when the vector gives the data as hex */
    uint16_t crc;
} r2s_vec_crc_t;

typedef struct {
    const char *where;
    const char *covers;
    const uint8_t *decoded;
    uint16_t decoded_len;
    const char *decoded_hex;
    const uint8_t *encoded; /* the delimiter included */
    uint16_t encoded_len;
    const char *encoded_hex;
    const char *reject; /* the rule a decoder rejects it by, or NULL; decoded is empty then */
} r2s_vec_rcobs_t;

typedef struct {
    const char *where;
    const char *covers;
    uint32_t value;
    const uint8_t *encoded;
    uint16_t encoded_len;
    const char *encoded_hex;
    const char *reject; /* the rule a decoder rejects it by, or NULL; value is 0 then */
} r2s_vec_varint_t;

typedef struct {
    const char *where;
    const char *covers;
    uint8_t topic_id;
    uint8_t seq;
    const uint8_t *payload;
    uint16_t payload_len;
    const char *payload_hex;
    const uint8_t *frame; /* the decoded frame before rCOBS (FRM-2) */
    uint16_t frame_len;
    const char *frame_hex;
    const uint8_t *wire; /* the encoded frame and its delimiter (FRM-1) */
    uint16_t wire_len;
    const char *wire_hex;
} r2s_vec_frames_t;

typedef struct {
    const char *where;
    const char *covers;
    const char *type;
    uint8_t narrow;
    const uint8_t *payload;
    uint16_t payload_len;
    const char *payload_hex;
    const uint8_t *encode; /* the struct bytes the vector's message holds */
    uint16_t encode_len;
    const uint8_t *decode; /* the struct bytes the payload decodes to */
    uint16_t decode_len;
} r2s_vec_messages_t;

/* One step of a handshake transcript: which side sent it, at which
 * millisecond, and the wire bytes with the delimiter. */
typedef struct {
    uint8_t from_host;
    uint32_t at_ms;
    const uint8_t *frame;
    uint16_t frame_len;
    const char *frame_hex; /* NULL when the step gives no hex text */
} r2s_vec_session_step_t;

/* A whole transcript: the file's name and its ordered steps. */
typedef struct {
    const char *where;
    const char *covers;
    const char *name;
    const r2s_vec_session_step_t *steps;
    uint16_t nsteps;
} r2s_vec_session_t;

/* One time vector case: the tick rate, the correction, the tick to read and
 * the time it makes: synced with sec and nanosec, or unsynced with both
 * zero. The widths keep a wraparound and a negative drift exact. */
typedef struct {
    const char *where;
    const char *covers;
    uint32_t tick_hz;
    uint32_t tick0;
    int32_t sec0;
    uint32_t nanosec0;
    int16_t drift_ppm;
    uint32_t tick;
    uint8_t synced;
    int32_t sec;
    uint32_t nanosec;
} r2s_vec_time_t;

/* One check per kind, linked in by the test executable: the runner
 * self-test, or the conformance test of the module that implements the kind. */
void r2s_vec_check_crc(const r2s_vec_crc_t *v);
void r2s_vec_check_frames(const r2s_vec_frames_t *v);
void r2s_vec_check_messages(const r2s_vec_messages_t *v);
void r2s_vec_check_rcobs(const r2s_vec_rcobs_t *v);
void r2s_vec_check_session(const r2s_vec_session_t *v);
void r2s_vec_check_time(const r2s_vec_time_t *v);
void r2s_vec_check_varint(const r2s_vec_varint_t *v);

#endif
