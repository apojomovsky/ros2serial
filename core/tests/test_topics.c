/* The flash-table topic accessors over the handwritten table: count, lookup by
 * index and by topic ID, and out-of-range access. */

#include "handwritten.h"
#include "r2s/r2s_topics.h"
#include "unity.h"

/* A second table proving configure replaces the binding instead of merging. */
static const r2s_topic_t single_topic[] = {
    {0x10u, 0x00u, {0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u}, "std_msgs/msg/Float32", "solo"},
};

/* The log_text hash the handwritten table stands in for. */
static const uint8_t log_text_hash[8] = {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u};

void test_topics_count_matches_configured_table(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_UINT8(3, r2s_topic_count());
}

void test_topics_get_by_index_returns_table_entries(void)
{
    const r2s_topic_t *battery;
    const r2s_topic_t *cmd_vel;
    const r2s_topic_t *log_text;

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    battery = r2s_topic_get(0u);
    cmd_vel = r2s_topic_get(1u);
    log_text = r2s_topic_get(2u);
    if (battery == NULL || cmd_vel == NULL || log_text == NULL) {
        TEST_FAIL_MESSAGE("bound table entry missing");
        return;
    }
    TEST_ASSERT_EQUAL_UINT8(R2S_HANDWRITTEN_BATTERY_ID, battery->topic_id);
    TEST_ASSERT_EQUAL_UINT8(R2S_HANDWRITTEN_CMD_VEL_ID, cmd_vel->topic_id);
    TEST_ASSERT_EQUAL_UINT8(R2S_HANDWRITTEN_LOG_TEXT_ID, log_text->topic_id);
    TEST_ASSERT_EQUAL_STRING("std_msgs/msg/Float32", battery->type_name);
    TEST_ASSERT_EQUAL_STRING("geometry_msgs/msg/Twist", cmd_vel->type_name);
    TEST_ASSERT_EQUAL_STRING("battery", battery->topic_name);
    TEST_ASSERT_EQUAL_STRING("cmd_vel", cmd_vel->topic_name);
    TEST_ASSERT_EQUAL_STRING("log_text", log_text->topic_name);
    TEST_ASSERT_EQUAL_UINT8(0u, battery->flags & 0x01u);
    TEST_ASSERT_EQUAL_UINT8(0x01u, cmd_vel->flags & 0x01u);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(log_text_hash, log_text->type_hash, 8u);
}

void test_topics_find_by_id_returns_index(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_UINT8(0u, r2s_topic_find(R2S_HANDWRITTEN_BATTERY_ID));
    TEST_ASSERT_EQUAL_UINT8(1u, r2s_topic_find(R2S_HANDWRITTEN_CMD_VEL_ID));
    TEST_ASSERT_EQUAL_UINT8(2u, r2s_topic_find(R2S_HANDWRITTEN_LOG_TEXT_ID));
}

void test_topics_unknown_id_returns_sentinel(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_topic_find(0u));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_topic_find(15u));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_topic_find(19u));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_topic_find(0xFFu));
}

void test_topics_get_past_count_returns_null(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_NULL(r2s_topic_get(3u));
    TEST_ASSERT_NULL(r2s_topic_get(0xFFu));
}

void test_topics_empty_table_has_no_topics(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_topics_configure(NULL, 0u));
    TEST_ASSERT_EQUAL_UINT8(0u, r2s_topic_count());
    TEST_ASSERT_NULL(r2s_topic_get(0u));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_topic_find(R2S_HANDWRITTEN_BATTERY_ID));
}

void test_topics_null_table_with_count_fails_unchanged(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_topics_configure(NULL, 1u));
    TEST_ASSERT_EQUAL_UINT8(3u, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(0u, r2s_topic_find(R2S_HANDWRITTEN_BATTERY_ID));
}

void test_topics_configure_replaces_table(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_topics_configure(single_topic, 1u));
    TEST_ASSERT_EQUAL_UINT8(1u, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(0u, r2s_topic_find(0x10u));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_topic_find(R2S_HANDWRITTEN_CMD_VEL_ID));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_UINT8(3u, r2s_topic_count());
}

void test_topics_count_above_max_is_refused(void)
{
    static r2s_topic_t big[R2S_TOPIC_MAX_COUNT + 1u];
    uint8_t i;

    for (i = 0u; i < R2S_TOPIC_MAX_COUNT + 1u; i++) {
        big[i].topic_id = i;
        big[i].type_name = "t";
        big[i].topic_name = "n";
    }
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_topics_configure(big, R2S_TOPIC_MAX_COUNT + 1u));
    TEST_ASSERT_EQUAL_UINT8(3u, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(0u, r2s_topic_find(R2S_HANDWRITTEN_BATTERY_ID));
}

void test_topics_nameless_entry_is_refused(void)
{
    static const r2s_topic_t no_type[] = {
        {0x10u, 0x00u, {0u}, NULL, "battery"},
    };
    static const r2s_topic_t no_name[] = {
        {0x10u, 0x00u, {0u}, "std_msgs/msg/Float32", NULL},
    };

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_topics_configure(no_type, 1u));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_topics_configure(no_name, 1u));
    TEST_ASSERT_EQUAL_UINT8(3u, r2s_topic_count());
}
