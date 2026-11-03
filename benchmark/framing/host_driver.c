/* Host driver for the framing benchmark: one argv front end over every
 * framer, so the pytest cross-checks the C against the Python models. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bm_api.h"
#include "bm_cobs.h"
#include "bm_crc_table.h"
#include "bm_rs.h"
#include "bm_slip.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_rcobs.h"

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static uint16_t unhex(const char *text, uint8_t *out, uint16_t cap)
{
    uint16_t len = (uint16_t)strlen(text);
    uint16_t i;
    if (len % 2u != 0u) {
        return 0xFFFFu;
    }
    len /= 2u;
    if (len > cap) {
        return 0xFFFFu;
    }
    for (i = 0u; i < len; i++) {
        int hi = hex_val(text[2u * i]);
        int lo = hex_val(text[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return 0xFFFFu;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return len;
}

static void show_hex(const uint8_t *data, uint16_t len)
{
    static const char digits[] = "0123456789abcdef";
    uint16_t i;
    for (i = 0u; i < len; i++) {
        putchar(digits[data[i] >> 4]);
        putchar(digits[data[i] & 0x0Fu]);
    }
    putchar('\n');
}

static int encode_one(const char *framer, const uint8_t *in, uint16_t in_len)
{
    uint8_t dec[BM_DEC_CAP];
    uint8_t wire[BM_WIRE_CAP];
    uint16_t wire_len = 0u;
    uint16_t crc;
    if (strcmp(framer, "rs") == 0) {
        if (in_len < 2u) {
            return 2;
        }
        wire_len = bm_rs_encode(in[0], in[1], in + 2u, (uint16_t)(in_len - 2u), wire, sizeof wire);
    } else {
        if (in_len + 2u > sizeof dec) {
            return 2;
        }
        memcpy(dec, in, in_len);
        crc = r2s_crc16_update_buf(R2S_CRC16_INIT, in, in_len);
        dec[in_len] = (uint8_t)crc;
        dec[in_len + 1u] = (uint8_t)(crc >> 8);
        if (strcmp(framer, "rcobs") == 0) {
            r2s_rcobs_encoder_t enc;
            uint16_t i;
            wire_len = 0u;
            r2s_rcobs_encoder_init(&enc);
            for (i = 0u; i < in_len + 2u; i++) {
                wire_len += r2s_rcobs_encode_byte(&enc, dec[i], wire + wire_len);
            }
            wire_len += r2s_rcobs_encode_end(&enc, wire + wire_len);
            wire[wire_len++] = 0x00u;
        } else if (strcmp(framer, "cobs") == 0) {
            wire_len = bm_cobs_encode(dec, (uint16_t)(in_len + 2u), wire, sizeof wire);
            wire[wire_len++] = 0x00u;
        } else if (strcmp(framer, "slip") == 0) {
            wire_len = bm_slip_encode(dec, (uint16_t)(in_len + 2u), wire, sizeof wire);
        } else {
            return 2;
        }
    }
    if (wire_len == 0u) {
        return 2;
    }
    show_hex(wire, wire_len);
    return 0;
}

static int decode_one(const char *framer, const uint8_t *wire, uint16_t wire_len)
{
    uint8_t buf[BM_WIRE_CAP];
    const uint8_t *dec = NULL;
    uint16_t dec_len = 0u;
    int status = -1;
    if (wire_len > sizeof buf) {
        return 2;
    }
    memcpy(buf, wire, wire_len);
    if (strcmp(framer, "rcobs") == 0) {
        status = r2s_rcobs_decode(buf, wire_len, (uint8_t **)&dec, &dec_len);
    } else if (strcmp(framer, "cobs") == 0) {
        status = bm_cobs_decode(buf, wire_len, &dec, &dec_len);
    } else if (strcmp(framer, "slip") == 0) {
        status = bm_slip_decode(buf, wire_len, &dec, &dec_len);
    } else {
        return 2;
    }
    if (status != 0) {
        printf("ERR\n");
        return 0;
    }
    if (r2s_crc16_update_buf(R2S_CRC16_INIT, dec, dec_len) != 0u) {
        printf("CRC ");
    } else {
        printf("OK ");
    }
    show_hex(dec, dec_len);
    return 0;
}

static int scan_stream(const char *framer, const uint8_t *stream, uint16_t len)
{
    uint16_t pos = 0u;
    uint8_t delimiter = (strcmp(framer, "slip") == 0) ? BM_SLIP_END : 0x00u;
    if (strcmp(framer, "rs") == 0) {
        while (pos < len) {
            uint16_t topic = 0u;
            uint8_t seq = 0u;
            const uint8_t *payload = NULL;
            uint16_t payload_len = 0u;
            uint16_t consumed = 0u;
            if (!bm_rs_find(stream + pos, (uint16_t)(len - pos), &topic, &seq, &payload, &payload_len,
                            &consumed)) {
                break;
            }
            printf("FRAME %04x %02x ", topic, seq);
            show_hex(payload, payload_len);
            pos += consumed;
        }
        printf("CONSUMED %u\n", pos);
        return 0;
    }
    if (strcmp(framer, "rcobs") != 0 && strcmp(framer, "cobs") != 0 && strcmp(framer, "slip") != 0) {
        return 2;
    }
    while (pos < len) {
        uint16_t end = pos;
        uint8_t seg[BM_WIRE_CAP];
        const uint8_t *dec = NULL;
        uint16_t dec_len = 0u;
        int status = -1;
        while (end < len && stream[end] != delimiter) {
            end++;
        }
        if (end >= len) {
            break;
        }
        if (end > pos) {
            memcpy(seg, stream + pos, end - pos);
            if (strcmp(framer, "rcobs") == 0) {
                status = r2s_rcobs_decode(seg, (uint16_t)(end - pos), (uint8_t **)&dec, &dec_len);
            } else if (strcmp(framer, "cobs") == 0) {
                status = bm_cobs_decode(seg, (uint16_t)(end - pos), &dec, &dec_len);
            } else {
                status = bm_slip_decode(seg, (uint16_t)(end - pos), &dec, &dec_len);
            }
            if (status != 0) {
                printf("DROP\n");
            } else if (r2s_crc16_update_buf(R2S_CRC16_INIT, dec, dec_len) != 0u) {
                printf("CRC ");
                show_hex(dec, dec_len);
            } else {
                printf("OK ");
                show_hex(dec, dec_len);
            }
        }
        pos = end + 1u;
    }
    printf("CONSUMED %u\n", pos);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t input[BM_WIRE_CAP];
    uint16_t in_len;
    if (strcmp(argv[1], "crc") == 0 && (argc == 3 || argc == 4)) {
        const char *hex = (argc == 4) ? argv[3] : argv[2];
        uint16_t crc_len = unhex(hex, input, sizeof input);
        if (crc_len == 0xFFFFu) {
            fprintf(stderr, "bad hex input\n");
            return 2;
        }
        printf("%04x\n", bm_crc16_table_update_buf(R2S_CRC16_INIT, input, crc_len));
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: host_driver <encode|decode|scan> <framer> <hex>\n");
        return 2;
    }
    in_len = unhex(argv[3], input, sizeof input);
    if (in_len == 0xFFFFu) {
        fprintf(stderr, "bad hex input\n");
        return 2;
    }
    if (strcmp(argv[1], "encode") == 0) {
        return encode_one(argv[2], input, in_len);
    }
    if (strcmp(argv[1], "decode") == 0) {
        return decode_one(argv[2], input, in_len);
    }
    if (strcmp(argv[1], "scan") == 0) {
        return scan_stream(argv[2], input, in_len);
    }
    fprintf(stderr, "unknown op %s\n", argv[1]);
    return 2;
}
