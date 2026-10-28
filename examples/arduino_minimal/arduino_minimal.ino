/* Minimal Arduino example: the generated pair on Uno and Mega, publishing
 * battery and counting cmd_vel frames. The generated header carries C
 * linkage, so the sketch includes it plainly; the port glue over Serial
 * lives in the library, which also counts the boots. */
#include <r2s_generated.h>

static uint32_t s_cmd_vel_count;
static uint32_t s_sent;

/* Declared with C linkage by the generated header, so this definition keeps
 * it with no extern "C" block; dispatch calls it from r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
    s_cmd_vel_count++;
}

void setup(void)
{
    Serial.begin(115200);
    if (r2s_session_configure("hello_arduino", r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
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
        std_msgs__msg__Float32 msg;

        msg.data = (float)s_sent;
        if (r2s_publish_battery(&msg) == R2S_OK) {
            s_sent++;
            last_pub = now;
        }
    }
}
