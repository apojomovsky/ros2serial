// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* The protocol version the core announces. */
#include "r2s/r2s.h"
#include "unity.h"

/* covers: GEN-2 */
void test_version_protocol_is_4(void)
{
  TEST_ASSERT_EQUAL_UINT8(4, R2S_PROTOCOL_VERSION);
}
