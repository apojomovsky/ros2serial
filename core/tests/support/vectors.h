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

/* One check per kind, linked in by the test executable: the runner
 * self-test, or the conformance test of the module that implements the kind. */
void r2s_vec_check_crc(const r2s_vec_crc_t *v);
void r2s_vec_check_frames(const r2s_vec_frames_t *v);
void r2s_vec_check_rcobs(const r2s_vec_rcobs_t *v);
void r2s_vec_check_varint(const r2s_vec_varint_t *v);

#endif
