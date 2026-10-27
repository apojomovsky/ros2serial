/* Power-up probe: the first r2s_init() without any reset must announce. It has
 * its own main, because the Unity setUp resets every module first. */
#include <stdio.h>

#include "fake_port.h"
#include "r2s/r2s.h"

int main(void)
{
    fake_port_reset();
    r2s_init();
    if (fake_port_tx_frame_count() != 1u || r2s_connected() != 0u) {
        fprintf(stderr, "powerup: expected one HELLO while unconnected\n");
        return 1;
    }
    return 0;
}
