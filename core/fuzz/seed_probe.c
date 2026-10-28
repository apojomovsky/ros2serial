/* Probe: every codec seed must decode through the fuzz table. The fuzzer
 * treats a refusal as a legal outcome, so a broken table or a mistyped seed
 * header would pass the seeds stage silently; this turns that drift into a
 * failure before any target runs. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "r2s/r2s_codec.h"
#include "r2s/r2s_port.h"

#include "fuzz_codec_table.h"

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}

uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

static uint8_t file_bytes[65535];

/* The table's own shape: records chained first to last with no gaps, nested
 * entries naming an earlier record ([DSC-12]). A shifted record fails here
 * with the table named, instead of decoding to nothing later. */
static int check_table(void)
{
    uint16_t first = 0u;
    uint16_t i;

    for (i = 0u; i < FUZZ_CODEC_TYPE_COUNT; i++) {
        const uint8_t *record = fuzz_types + (uint16_t)(5u * i);
        uint16_t rec_first = (uint16_t)((uint16_t)record[2] | ((uint16_t)record[3] << 8u));
        uint8_t count = record[4];
        uint16_t e;

        if (rec_first != first) {
            fprintf(stderr, "seed_probe: record %u starts at %u, not %u\n", i, rec_first, first);
            return 1;
        }
        for (e = 0u; e < count; e++) {
            const uint8_t *entry = fuzz_fields + (uint16_t)(8u * (first + e));
            uint16_t aux = (uint16_t)((uint16_t)entry[6] | ((uint16_t)entry[7] << 8u));

            if (entry[0] == R2S_CODEC_NEST && aux >= i) {
                fprintf(stderr, "seed_probe: record %u names later record %u\n", i, aux);
                return 1;
            }
        }
        first += count;
    }
    if (first != 29u) {
        fprintf(stderr, "seed_probe: %u entries, not 29\n", first);
        return 1;
    }
    return 0;
}
int main(int argc, char **argv)
{
    int failures = check_table();
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: seed_probe SEED...\n");
        return 2;
    }
    for (i = 1; i < argc; i++) {
        static fuzz_msg_union_t msg;
        FILE *file = fopen(argv[i], "rb");
        size_t len;
        uint16_t type;
        uint8_t narrow;
        uint8_t field = 0u;

        if (file == NULL) {
            fprintf(stderr, "seed_probe: cannot open %s\n", argv[i]);
            failures++;
            continue;
        }
        len = fread(file_bytes, 1u, sizeof file_bytes, file);
        fclose(file);
        if (len < 2u || len == sizeof file_bytes) {
            fprintf(stderr, "seed_probe: %s has no headed payload\n", argv[i]);
            failures++;
            continue;
        }
        /* The same header the fuzz target reads. */
        type = (uint16_t)(file_bytes[0] % FUZZ_CODEC_TYPE_COUNT);
        narrow = (uint8_t)(file_bytes[1] & 1u);
        memset(&msg, 0, sizeof msg);
        if (r2s_codec_decode(&FUZZ_CODEC_TABLE, type, file_bytes + 2u, (uint16_t)(len - 2u), narrow, &msg,
                             &field) != R2S_OK) {
            fprintf(stderr, "seed_probe: %s does not decode\n", argv[i]);
            failures++;
        }
    }
    return failures != 0;
}
