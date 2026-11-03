/* bm_crc_table: CRC-16/MCRF4XX through a 256 entry table for the benchmark.
 * Same wire bytes as the shipped bitwise routine, traded flash for cycles. */
#ifndef BM_CRC_TABLE_H
#define BM_CRC_TABLE_H

#include <stdint.h>

/* Returns the CRC after feeding one more byte to crc, like
 * r2s_crc16_update but through the table. */
uint16_t bm_crc16_table_update(uint16_t crc, uint8_t byte);

/* Returns the CRC after feeding len bytes of data to crc. data may be NULL
 * when len is 0. */
uint16_t bm_crc16_table_update_buf(uint16_t crc, const uint8_t *data, uint16_t len);

#endif
