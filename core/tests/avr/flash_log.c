/* AVR flash check firmware: R2S_FLASH tables and flash-string log calls. The
 * check builds it with and without the log lines: equal SRAM proves the
 * strings stayed out of RAM, and avr-strings proves the lines survived. */
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_topics.h"

/* Descriptor-shaped tables, as the generator emits them (G7). */
static const uint8_t R2S_FLASH check_types[] = {0x03u, 0x00u, 0x00u, 0x00u, 0x01u};
static const uint8_t R2S_FLASH check_fields[] = {0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
static const char R2S_FLASH check_type_name[] = "std_msgs/msg/Float32";
static const char R2S_FLASH check_topic_name[] = "battery";
static const r2s_topic_t R2S_FLASH check_topics[] = {
    {0x10u,
     0x03u,
     {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u},
     check_type_name,
     check_topic_name}};

static volatile uint8_t sink;

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    (void)buf;
    (void)max;
    return 0u;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    if (len > 0u) {
        sink = buf[len - 1u];
    }
}

uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
    return 0u;
}

uint32_t r2s_port_boot_id(void)
{
    return 1u;
}

/* Reads every table byte through the flash reader, so the tables and the
 * reader stay live in both builds. */
static void touch_tables(void)
{
    uint8_t i;

    for (i = 0u; i < sizeof check_types; i++) {
        sink = R2S_FLASH_READ_BYTE(check_types + i);
    }
    for (i = 0u; i < sizeof check_fields; i++) {
        sink = R2S_FLASH_READ_BYTE(check_fields + i);
    }
    sink = R2S_FLASH_READ_BYTE(&check_topics[0].topic_id);
    sink = R2S_FLASH_READ_BYTE(&check_topics[0].flags);
}

int main(void)
{
    touch_tables();
#ifdef R2S_AVR_CHECK_LOGS
    /* A variable value: the no-op macros of a tier without logging must keep
     * it used, or the build fails on the unused variable. */
    uint16_t adc = (uint16_t)(sink + 1u);

    R2S_LOG_INFO("battery low log line");
    R2S_LOG_WARN_U16("adc counts=", adc);
    R2S_LOG_ERROR("link lost log line");
#endif
    return 0;
}
