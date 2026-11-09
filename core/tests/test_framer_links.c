/* The framer with several live instances: one link per instance, each with
 * its own bytes, frames and counters, sharing nothing but the port seam. */
#include <string.h>

#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];
static uint8_t other[WIRE_MAX];

static r2s_framer_t first;
static r2s_framer_t second;

/* covers: FRM-2, FRM-13 */
void test_framer_links_decode_on_separate_links_without_sharing(void)
{
  const uint8_t payload_a[] = {0x01, 0x02};
  const uint8_t payload_b[] = {0x03, 0x04, 0x05};
  r2s_frame_t frame_a;
  r2s_frame_t frame_b;
  uint16_t len_a = r2s_test_frame(wire, 16, 0, payload_a, sizeof payload_a);
  uint16_t len_b = r2s_test_frame(other, 17, 4, payload_b, sizeof payload_b);

  r2s_framer_init_inst(&first, 0u);
  r2s_framer_init_inst(&second, 1u);
  fake_port_set_tick_link(0u, 77u);
  fake_port_set_tick_link(1u, 78u);
  fake_port_queue_rx_link(0u, wire, len_a);
  fake_port_queue_rx_link(1u, other, len_b);

  TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll_inst(&first, &frame_a));
  TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll_inst(&second, &frame_b));

    /* Both frames wait unreleased, so shared state would have aliased them. */
  TEST_ASSERT_EQUAL_HEX8(16, frame_a.data[0]);
  TEST_ASSERT_EQUAL_HEX8(17, frame_b.data[0]);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(payload_a, frame_a.data + 2, sizeof payload_a);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(payload_b, frame_b.data + 2, sizeof payload_b);
  TEST_ASSERT_EQUAL_HEX32(77, frame_a.tick);
  TEST_ASSERT_EQUAL_HEX32(78, frame_b.tick);
  TEST_ASSERT_EQUAL_HEX16(0, r2s_crc16_update_buf(R2S_CRC16_INIT, frame_a.data, frame_a.len));
  TEST_ASSERT_EQUAL_HEX16(0, r2s_crc16_update_buf(R2S_CRC16_INIT, frame_b.data, frame_b.len));

  r2s_framer_release_inst(&first);
  r2s_framer_release_inst(&second);
  TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll_inst(&first, &frame_a));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll_inst(&second, &frame_b));
}

/* covers: FRM-13, FRM-14 */
void test_framer_links_a_partial_frame_waits_only_on_its_own_link(void)
{
  const uint8_t payload[] = {0x2a};
  r2s_frame_t frame;
  uint16_t len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);
  uint16_t other_len = r2s_test_frame(other, 17, 0, payload, sizeof payload);

  r2s_framer_init_inst(&first, 0u);
  r2s_framer_init_inst(&second, 1u);
  fake_port_queue_rx_link(0u, wire, (uint16_t)(len - 1u));
  fake_port_queue_rx_link(1u, other, other_len);

  TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll_inst(&first, &frame));
  TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll_inst(&second, &frame));
  TEST_ASSERT_EQUAL_HEX8(17, frame.data[0]);
  r2s_framer_release_inst(&second);

  fake_port_queue_rx_link(0u, &wire[len - 1u], 1);
  TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll_inst(&first, &frame));
  TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
  r2s_framer_release_inst(&first);
}

/* covers: FRM-6, FRM-8, FRM-12, HB-4, HB-5, HB-6, HB-7 */
void test_framer_links_drops_and_counters_stay_on_their_link(void)
{
  const uint8_t payload[] = {0x01};
  static uint8_t stream[R2S_FRAMER_RX_CAPACITY + 3u];
  r2s_frame_t frame;
  uint16_t len;
  const r2s_framer_counters_t *counters;

  r2s_framer_init_inst(&first, 0u);
  r2s_framer_init_inst(&second, 1u);

  memset(stream, 0xaa, sizeof stream);
  stream[sizeof stream - 1u] = 0x00u;
  fake_port_queue_rx_link(1u, stream, sizeof stream);
  len = r2s_test_frame_bad_crc(wire, 16, 0, payload, sizeof payload);
  fake_port_queue_rx_link(1u, wire, len);
  TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll_inst(&second, &frame));

  counters = r2s_framer_counters_inst(&second);
  TEST_ASSERT_EQUAL_UINT16(1, counters->rx_overflows);
  TEST_ASSERT_EQUAL_UINT16(1, counters->crc_errors);
  TEST_ASSERT_EQUAL_UINT16(2, counters->dropped_frames);
  TEST_ASSERT_EQUAL_UINT16(0, counters->rx_frames);

  counters = r2s_framer_counters_inst(&first);
  TEST_ASSERT_EQUAL_UINT16(0, counters->rx_overflows);
  TEST_ASSERT_EQUAL_UINT16(0, counters->crc_errors);
  TEST_ASSERT_EQUAL_UINT16(0, counters->dropped_frames);
  TEST_ASSERT_EQUAL_UINT16(0, counters->rx_frames);

  r2s_framer_count_refused_inst(&second);
  TEST_ASSERT_EQUAL_UINT16(3, r2s_framer_counters_inst(&second)->dropped_frames);
  TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters_inst(&first)->dropped_frames);

  len = r2s_test_frame(other, 16, 0, payload, sizeof payload);
  fake_port_queue_rx_link(0u, other, len);
  TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll_inst(&first, &frame));
  TEST_ASSERT_EQUAL_UINT16(1, r2s_framer_counters_inst(&first)->rx_frames);
  TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters_inst(&second)->rx_frames);
  r2s_framer_release_inst(&first);
}

/* covers: FRM-12 */
void test_framer_links_error_reports_stay_on_their_link(void)
{
  static uint8_t stream[R2S_FRAMER_RX_CAPACITY + 3u];
  r2s_frame_t frame;
  uint16_t overflow = 0u;
  uint16_t too_large = 0u;
  uint8_t topic = 0u;

  r2s_framer_init_inst(&first, 0u);
  r2s_framer_init_inst(&second, 1u);

  memset(stream, 0xaa, sizeof stream);
  stream[sizeof stream - 1u] = 0x00u;
  fake_port_queue_rx_link(1u, stream, sizeof stream);
  TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll_inst(&second, &frame));

  r2s_framer_take_error_reports_inst(&second, &overflow, &too_large, &topic);
  TEST_ASSERT_EQUAL_UINT16(1, overflow);

  r2s_framer_take_error_reports_inst(&first, &overflow, &too_large, &topic);
  TEST_ASSERT_EQUAL_UINT16(0, overflow);
  TEST_ASSERT_EQUAL_UINT16(0, too_large);
}
