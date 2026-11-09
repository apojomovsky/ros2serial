/* action: the registry behind the ROS 2-named action server API. It holds
 * the RAM action table the core announces and the records the goal exchange
 * dispatches through. The core itself keeps no function pointer; the
 * callbacks live here. Without R2S_WITH_ACTIONS the core carries no actions
 * channel, so this stays an empty translation unit and the sketches that
 * need none are untouched. */
#ifdef R2S_WITH_ACTIONS

#include "r2s/action.hpp"

#include "r2s/r2s_actions.h"

/* The RAM the actions cost, named so a size report can find it: the table
 * the core announces, and the records beside it. */
extern "C" {
r2s_action_t r2s_action_table[R2S_ACTION_MAX_COUNT];
}

/* One server's dispatch, kept beside the entry it announces. */
struct action_record {
    void *server;
    void (*dispatch_goal)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t);
    void (*dispatch_cancel)(void *, uint8_t, uint8_t);
};

extern "C" {
action_record r2s_action_servers[R2S_ACTION_MAX_COUNT];
}

namespace
{
/* The registry: the count, and the flag the firmware asks about. */
struct registry {
    uint8_t count;
    uint8_t error;
};

registry reg;
} // namespace

namespace r2s
{
namespace action
{
namespace detail
{
uint8_t register_server(const char *action_name, const char *type_name, const uint8_t *type_hash,
                        void *record,
                        void (*dispatch_goal)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t),
                        void (*dispatch_cancel)(void *, uint8_t, uint8_t))
{
    uint8_t i;
    r2s_action_t entry;

    if (reg.count >= R2S_ACTION_MAX_COUNT) {
        reg.error = 1u;
        return 0xFFu;
    }
    if (action_name == nullptr || type_name == nullptr || type_hash == nullptr || record == nullptr ||
        dispatch_goal == nullptr || dispatch_cancel == nullptr) {
        reg.error = 1u;
        return 0xFFu;
    }
    for (i = 0u; i < 8u; i++) {
        entry.type_hash[i] = type_hash[i];
    }
    entry.type_name = type_name;
    entry.action_name = action_name;
    r2s_action_table[reg.count] = entry;
    if (r2s_actions_configure_ram(r2s_action_table, (uint8_t)(reg.count + 1u)) != R2S_OK) {
        reg.error = 1u;
        return 0xFFu;
    }
    r2s_action_servers[reg.count].server = record;
    r2s_action_servers[reg.count].dispatch_goal = dispatch_goal;
    r2s_action_servers[reg.count].dispatch_cancel = dispatch_cancel;
    return reg.count++;
}

bool registration_error(void)
{
    return reg.error != 0u;
}
} // namespace detail
} // namespace action
} // namespace r2s

/* The hooks the core calls for the goal exchange. The dispatch keeps the
 * server's index, so a goal for an unknown registration is refused instead
 * of running none. */
extern "C" void r2s_action_on_goal(uint8_t index, uint8_t goal_seq, const uint8_t *payload, uint16_t len)
{
    if (index >= reg.count) {
        (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_REJECTED);
        return;
    }
    r2s_action_servers[index].dispatch_goal(r2s_action_servers[index].server, index, goal_seq, payload, len);
}

extern "C" void r2s_action_on_cancel(uint8_t index, uint8_t goal_seq)
{
    if (index >= reg.count) {
        (void)r2s_actions_answer_cancel(index, goal_seq, R2S_ACT_CANCEL_REJECTED);
        return;
    }
    r2s_action_servers[index].dispatch_cancel(r2s_action_servers[index].server, index, goal_seq);
}

#ifdef R2S_TESTING
extern "C" void r2s_action_test_reset(void)
{
    uint8_t i;

    for (i = 0u; i < R2S_ACTION_MAX_COUNT; i++) {
        r2s_action_table[i].type_hash[0] = 0u;
        r2s_action_table[i].type_name = nullptr;
        r2s_action_table[i].action_name = nullptr;
        r2s_action_servers[i].server = nullptr;
        r2s_action_servers[i].dispatch_goal = nullptr;
        r2s_action_servers[i].dispatch_cancel = nullptr;
    }
    reg.count = 0u;
    reg.error = 0u;
}
#endif
#else
/* ISO C++ needs at least one declaration in a translation unit. */
typedef int r2s_action_unused;
#endif
