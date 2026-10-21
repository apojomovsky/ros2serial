/* rCOBS: the framing that removes every 0x00 from a frame, so a 0x00 can
 * delimit it (spec/protocol.md, section 4). Not part of the firmware API: the
 * core's framing uses it, and so does the bridge, which links rcobs.c for its
 * own framing. */
#ifndef R2S_RCOBS_H
#define R2S_RCOBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a decoder refused an encoding. Each one names the rule of
 * spec/protocol.md, section 4, that the encoding breaks. */
#define R2S_RCOBS_OK 0
#define R2S_RCOBS_ERR_CODE (-1)     /* COB-5 */
#define R2S_RCOBS_ERR_TRAILING (-2) /* COB-6 */

/* The most bytes one input byte, or the end of an encoding, can produce. */
#define R2S_RCOBS_MAX_EMIT 2u

/* The state of a streaming encoder. It is the run counter of COB-1 and
 * nothing else, so the encoder needs no buffer and no lookahead. */
typedef struct {
    uint8_t run; /* the code byte of the current group, 1 to 0xFF */
} r2s_rcobs_encoder_t;

/* Starts an encoding. The encoder must be started before the first byte. */
void r2s_rcobs_encoder_init(r2s_rcobs_encoder_t *enc);

/* Appends one byte to the encoding. The bytes it produces go to out, which
 * must hold R2S_RCOBS_MAX_EMIT bytes, and their number is returned. A byte of
 * 0x00 ends the current group, and a group of 254 bytes ends itself, so one
 * input byte produces at most two output bytes. */
uint8_t r2s_rcobs_encode_byte(r2s_rcobs_encoder_t *enc, uint8_t byte, uint8_t *out);

/* Ends the encoding and writes the last code byte to out, which must hold
 * R2S_RCOBS_MAX_EMIT bytes. It returns how many bytes it wrote, which is one.
 * The delimiter that follows a frame on the wire is the framer's, not the
 * encoder's. */
uint8_t r2s_rcobs_encode_end(r2s_rcobs_encoder_t *enc, uint8_t *out);

/* Decodes an encoded frame in place: buf holds len bytes of the encoding,
 * without its delimiter, and the decoded frame is written over it, right
 * aligned, so the decoded frame ends where the encoded one ended. The address
 * of its first byte is written to decoded, and the length of the decoded
 * frame to decoded_len. The function returns R2S_RCOBS_OK, or the error of
 * the rule the encoding breaks: R2S_RCOBS_ERR_TRAILING for COB-6, which is
 * checked before any code is read, and R2S_RCOBS_ERR_CODE for COB-5. The
 * buffer and the length are left undefined when the encoding is refused. The
 * length comes back from the decoder because a caller cannot work it out: a
 * pointer difference is 16 bits on an AVR, and a frame may be longer. */
int8_t r2s_rcobs_decode(uint8_t *buf, uint16_t len, uint8_t **decoded, uint16_t *decoded_len);

#ifdef __cplusplus
}
#endif

#endif
