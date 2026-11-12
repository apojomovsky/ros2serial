// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* HELLO feature bits of a services-only build: bit 6 rides with the
 * reliable bit ([CTL-2]), so the bridge lets the handshake through. */
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

/* The services bit rides with the reliable bit; nothing else is on in
 * this build, apart from the narrow bit on 32-bit-double hosts. */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define EXPECTED_FEATURES 0x58u
#else
#define EXPECTED_FEATURES 0x48u
#endif

static const r2s_topic_t one_topic[] = {
  {0x10u,
    0x00u,
    {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u},
    "std_msgs/msg/Float32",
    "float"},
};

/* Checks the index-th captured frame against the wire form of its topic,
 * seq and payload, the same comparison the frames conformance test makes.
 */
static void assert_tx(
  uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
  uint16_t payload_len)
{
  static uint8_t expected[WIRE_MAX];
  char label[64];
  uint16_t encoded_len;
  const uint8_t *encoded;
  uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

  snprintf(label, sizeof label, "tx frame %u", index);
  encoded = fake_port_tx_frame(index, &encoded_len);
  TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    /* The captured frame stops at its delimiter, which the built wire keeps. */
  R2S_ASSERT_BYTES(expected, (uint16_t)(expected_len - 1u), encoded, encoded_len, label);
}

/* covers: CTL-2 */
void test_services_only_hello_sets_services_and_reliable(void)
{
  static uint8_t expected[64];
  const char *name = "node";
  size_t name_len = strlen(name);
  uint8_t varint[R2S_VARINT_MAX_BYTES];
  uint8_t varint_len = r2s_varint_encode((uint16_t)name_len, varint);
  uint16_t len = 0u;

  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
  r2s_init();
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  expected[len++] = 0x02u;
  expected[len++] = R2S_PROTOCOL_VERSION;
  expected[len++] = 0x00u;
  expected[len++] = 0x00u;
  expected[len++] = 0x00u;
  expected[len++] = 0x00u;
  expected[len++] = 0x00u;
  expected[len++] = (uint8_t)(R2S_MAX_FRAME & 0xFFu);
  expected[len++] = (uint8_t)((R2S_MAX_FRAME >> 8u) & 0xFFu);
  expected[len++] = (uint8_t)(R2S_FRAMER_RX_CAPACITY & 0xFFu);
  expected[len++] = (uint8_t)((R2S_FRAMER_RX_CAPACITY >> 8u) & 0xFFu);
  expected[len++] = (uint8_t)(R2S_TICK_HZ & 0xFFu);
  expected[len++] = (uint8_t)((R2S_TICK_HZ >> 8u) & 0xFFu);
  expected[len++] = (uint8_t)((R2S_TICK_HZ >> 16u) & 0xFFu);
  expected[len++] = (uint8_t)((R2S_TICK_HZ >> 24u) & 0xFFu);
  expected[len++] = EXPECTED_FEATURES;
  expected[len++] = 0x01u;
  memcpy(expected + len, varint, varint_len);
  len = (uint16_t)(len + varint_len);
  memcpy(expected + len, name, name_len);
  len = (uint16_t)(len + name_len);
  assert_tx(0, 0, 0, expected, len);
}
