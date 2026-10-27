/* Minimal X-macro example: the bundled library on Uno and Mega, publishing
 * battery and greeting and counting cmd_vel and joints frames. The sketch
 * declares its topics in an R2S_TOPICS list, so there is no r2s.yaml and no
 * generator step; the bundled descriptors compile in the library, which
 * never sees the list. The header is C99, while the sketch compiles as C++,
 * so the include and every function the core calls carry C linkage. */
#include <EEPROM.h>

#define R2S_NODE_NAME "hello_xmacro"
#define R2S_TOPICS(X) \
    X(PUB, battery, std_msgs, Float32) \
    X(PUB, greeting, std_msgs, String) \
    X(SUB, cmd_vel, geometry_msgs, Twist) \
    X(SUB, joints, sensor_msgs, JointState)

/* The header carries C linkage, so the sketch includes it plainly; only the
 * port functions and handlers below still need their own extern "C" block.
 * The core needs the bundled limits as defines (R2S_WITH_CODEC and the
 * R2S_MAX_* values from r2s_bundled.h); the example build passes them
 * through compiler.c.extra_flags, since a library sets none of its own. */
#include <ros2serial.h>

static uint32_t s_boot_id;
static uint32_t s_cmd_vel_count;
static uint32_t s_joints_count;
static uint32_t s_sent;

/* C linkage: the core is C99 and resolves these names unmangled. */
extern "C" {
uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    uint16_t count = 0u;

    while (count < max && Serial.available() > 0) {
        buf[count] = (uint8_t)Serial.read();
        count++;
    }
    return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    Serial.write(buf, len);
}

/* Serial.write blocks for space, so the port never refuses a byte. */
uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

/* The tick as milliseconds; micros() resolves 4 us, fine for the 1 ms tick. */
uint32_t r2s_port_tick(void)
{
    return (uint32_t)(micros() / 1000UL);
}

/* A counter in the first EEPROM cell, so every boot announces a new ID. */
uint32_t r2s_port_boot_id(void)
{
    return s_boot_id;
}

/* Handle cmd_vel and joints; dispatch calls them from r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
    s_cmd_vel_count++;
}

void r2s_on_joints(const sensor_msgs__msg__JointState *msg)
{
    (void)msg;
    s_joints_count++;
}
} /* extern "C" */

void setup(void)
{
    Serial.begin(115200);
    EEPROM.get(0, s_boot_id);
    s_boot_id += 1u;
    EEPROM.put(0, s_boot_id);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}

void loop(void)
{
    static uint32_t last_pub;
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    if (r2s_connected() == 0u) {
        return;
    }
    if ((uint32_t)(now - last_pub) >= 100u) {
        std_msgs__msg__Float32 battery = {(float)s_sent};
        std_msgs__msg__String greeting = {0};

        if (r2s_publish_battery(&battery) == R2S_OK && r2s_publish_greeting(&greeting) == R2S_OK) {
            s_sent++;
            last_pub = now;
        }
    }
}
