/* action: the ROS 2-named action server API. Servers are registered while the
 * firmware runs, into the core's RAM action table, so a sketch needs no
 * generated action list. The core reaches the goal exchange through two
 * upcalls this layer implements, and the core itself keeps no function
 * pointer. A sketch with an R2S_ACTIONS list declares its servers as rows
 * instead, one row per runtime declaration. */
#ifndef R2S_ACTION_HPP
#define R2S_ACTION_HPP

#ifndef R2S_WITH_ACTIONS
#error "The action server needs R2S_WITH_ACTIONS: the core gates the actions channel on it."
#endif

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_actions.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"

/* Whether the build travels narrow on every action, as it does on every
 * topic: a build whose double is 32 bits encodes float64 fields as binary32.
 * Actions announce no flags, so the width is the build's own (ENC-12). */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_ACTION_NARROW 1u
#else
#define R2S_ACTION_NARROW 0u
#endif

namespace r2s
{
/* What every message type must provide: the descriptor table that encodes
 * and decodes it, its index in that table, its type name and the first
 * eight bytes of its type hash. The message headers specialize this
 * template, so an action names types and never tables. */
template <typename T> struct msg_traits;

namespace action
{
/* What every action type must provide: the goal, result and feedback message
 * types the sketch's action struct names. The generated action headers name
 * them as nested types, so a sketch names an action and never a part. */
template <typename T> struct traits;

/* The goal decision the handle_goal callback returns: run the goal or
 * refuse it. There is no deferral: one active goal per action leaves no
 * queue, so a second goal is answered busy without reaching the callback. */
enum class GoalResponse : uint8_t { REJECT = 1u, ACCEPT_AND_EXECUTE = 2u };

/* The cancel decision the handle_cancel callback returns: stop the goal or
 * let it run. */
enum class CancelResponse : uint8_t { REJECT = 1u, ACCEPT = 2u };

namespace detail
{
/* One server's typed storage and callbacks. The registry keeps the record,
 * so the upcalls stay typed while the core sees only plain functions. */
template <typename T> struct server_record {
    T goal;
};
} // namespace detail

namespace detail
{
/* Registers one server's action and callbacks, binding the core table as it
 * goes, and hands back the server's index or 0xFF. */
uint8_t register_server(const char *action_name, const char *type_name, const uint8_t *type_hash,
                        void *record,
                        void (*dispatch_goal)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t),
                        void (*dispatch_cancel)(void *, uint8_t, uint8_t));
} // namespace detail

/* The handle to the running goal, owned by its server: one goal per action,
 * so one handle per server. The application answers the goal through it,
 * from the accepted callback or later. */
template <typename T> class Server;

/* IWYU: the handle names the server that owns it. */
template <typename T> class ServerGoalHandle
{
  public:
    /* The accepted goal, as the upcall decoded it. Valid while the goal
     * runs; the server owns the storage. */
    const typename T::Goal &get_goal() const
    {
        return *m_goal;
    }

    /* Sends this feedback for the running goal best effort. Fails while the
     * goal is not running, sending nothing. */
    r2s_status_t publish_feedback(const typename T::Feedback &msg)
    {
        r2s_codec_out_t out = {m_encode, R2S_MAX_FRAME, 0u, 0u};
        uint8_t field = 0xFFu;

        if (r2s_codec_encode(msg_traits<typename T::Feedback>::table(),
                             msg_traits<typename T::Feedback>::type_index(), &msg, R2S_ACTION_NARROW, &out,
                             &field) != R2S_OK) {
            return R2S_ERR_BOUNDS;
        }
        return r2s_actions_feedback(m_index, m_seq, m_encode, out.len);
    }

    /* True once an accepted cancel arrived for the running goal. */
    bool is_canceling() const
    {
        return m_canceling;
    }

    /* Ends the running goal with this result: succeeded, aborted, or
     * canceled after an accepted cancel. Each fails while the goal is not
     * running, sending nothing. */
    r2s_status_t succeed(const typename T::Result &msg)
    {
        return send_result(msg, R2S_ACT_SUCCEEDED);
    }
    r2s_status_t abort(const typename T::Result &msg)
    {
        return send_result(msg, R2S_ACT_ABORTED);
    }
    r2s_status_t canceled(const typename T::Result &msg)
    {
        return send_result(msg, R2S_ACT_CANCELED);
    }

  private:
    friend class Server<T>;

    ServerGoalHandle() : m_goal(NULL), m_encode(NULL), m_index(0xFFu), m_seq(0u), m_canceling(false)
    {
    }

    r2s_status_t send_result(const typename T::Result &msg, uint8_t outcome)
    {
        r2s_codec_out_t out = {m_encode, R2S_MAX_FRAME, 0u, 0u};
        uint8_t field = 0xFFu;

        if (r2s_codec_encode(msg_traits<typename T::Result>::table(),
                             msg_traits<typename T::Result>::type_index(), &msg, R2S_ACTION_NARROW, &out,
                             &field) != R2S_OK) {
            return R2S_ERR_BOUNDS;
        }
        return r2s_actions_result(m_index, m_seq, outcome, m_encode, out.len);
    }

    const typename T::Goal *m_goal;
    uint8_t *m_encode;
    uint8_t m_index;
    uint8_t m_seq;
    bool m_canceling;
};

/* The MCU side of one action: a minimal action server with one active goal,
 * mirroring rclcpp_action::Server. The constructor registers the action
 * against the core table; the handle_goal callback answers each new goal,
 * handle_cancel answers each cancel, and handle_accepted receives the
 * running goal. Capture-less lambdas convert to the function pointers the
 * callbacks take. Keep the server alive while the node spins: the registry
 * keeps the address of its record. */
template <typename T> class Server
{
  public:
    Server(const char *action_name, GoalResponse (*goal_cb)(const typename T::Goal &msg),
           CancelResponse (*cancel_cb)(const typename T::Goal &msg),
           void (*accepted_cb)(ServerGoalHandle<T> &handle))
        : m_index(0xFFu), m_goal_cb(goal_cb), m_cancel_cb(cancel_cb), m_accepted_cb(accepted_cb)
    {
        m_index = detail::register_server(action_name, traits<T>::type_name(), traits<T>::type_hash(), this,
                                          &dispatch_goal, &dispatch_cancel);
        m_handle.m_goal = &m_goal;
        m_handle.m_encode = m_encode;
        m_handle.m_index = m_index;
    }

    /* The registry index of this server, or 0xFF when registration failed. */
    uint8_t index() const
    {
        return m_index;
    }

  private:
    Server(const Server &);
    Server &operator=(const Server &);

    static void dispatch_goal(void *record, uint8_t index, uint8_t goal_seq, const uint8_t *payload,
                              uint16_t len)
    {
        Server<T> *self = static_cast<Server<T> *>(record);
        uint8_t field = 0xFFu;

        switch (r2s_codec_decode(msg_traits<typename T::Goal>::table(),
                                 msg_traits<typename T::Goal>::type_index(), payload, len, R2S_ACTION_NARROW,
                                 &self->m_goal, &field)) {
        case R2S_OK:
            break;
        case R2S_ERR_BOUNDS:
            r2s_error_note(R2S_ERROR_BOUND, 8u, index, 1u);
            r2s_framer_count_refused();
            (void)r2s_actions_answer_goal(index, goal_seq, (uint8_t)GoalResponse::REJECT);
            return;
        default:
            r2s_framer_count_refused();
            (void)r2s_actions_answer_goal(index, goal_seq, (uint8_t)GoalResponse::REJECT);
            return;
        }
        GoalResponse decision = self->m_goal_cb(self->m_goal);
        if (r2s_actions_answer_goal(index, goal_seq, (uint8_t)decision) != R2S_OK) {
            return;
        }
        if (decision == GoalResponse::ACCEPT_AND_EXECUTE) {
            self->m_handle.m_seq = goal_seq;
            self->m_handle.m_canceling = false;
            self->m_accepted_cb(self->m_handle);
        }
    }

    static void dispatch_cancel(void *record, uint8_t index, uint8_t goal_seq)
    {
        Server<T> *self = static_cast<Server<T> *>(record);
        CancelResponse decision = self->m_cancel_cb(self->m_goal);

        if (r2s_actions_answer_cancel(index, goal_seq, (uint8_t)decision) != R2S_OK) {
            return;
        }
        if (decision == CancelResponse::ACCEPT) {
            self->m_handle.m_canceling = true;
        }
    }

    uint8_t m_index;
    GoalResponse (*m_goal_cb)(const typename T::Goal &msg);
    CancelResponse (*m_cancel_cb)(const typename T::Goal &msg);
    void (*m_accepted_cb)(ServerGoalHandle<T> &handle);
    typename T::Goal m_goal;
    ServerGoalHandle<T> m_handle;
    uint8_t m_encode[R2S_MAX_FRAME];
};

namespace detail
{
/* True when the firmware asked for something the registry refused: a table
 * the session would not bind, or more actions than the core holds. */
bool registration_error(void);
} // namespace detail

/* True when the firmware asked for something the registry refused. */
inline bool registration_error()
{
    return detail::registration_error();
}

} // namespace action
} // namespace r2s

/* One static row: the server object the row declares, registering like the
 * runtime one. A test expands its own list with this macro in function
 * scope; a sketch declares R2S_ACTIONS rows and the header expands them.
 * Beside the runtime declaration `Server<Type> obj(name, ...)` the row
 * reads `X(Type, obj, name, ...)`. */
#define R2S_ACTION_X_SERVER(Type, obj, name, goal_cb, cancel_cb, accepted_cb)                                \
    ::r2s::action::Server<Type> obj(name, goal_cb, cancel_cb, accepted_cb);

/* A sketch with a static action list declares its rows before including this
 * header; the header expands them into the server objects. Each row maps
 * 1-to-1 to a runtime declaration. */
#ifdef R2S_ACTIONS
R2S_ACTIONS(R2S_ACTION_X_SERVER)
#endif

#ifdef R2S_TESTING
/* Host test builds only: empties the registry, so one binary runs many
 * tests from the same power-up state. */
extern "C" void r2s_action_test_reset(void);
#endif

#endif
