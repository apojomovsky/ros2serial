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

/// Set when the build's double is 32 bits, so every action travels narrow.
/**
 * A build whose double is 32 bits encodes float64 fields as binary32.
 * Actions announce no flags, so the width is the build's own (ENC-12).
 */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_ACTION_NARROW 1u
#else
/// Clear when the build's double is 64 bits; actions travel full width.
#define R2S_ACTION_NARROW 0u
#endif

namespace r2s
{
template<typename T>
/// What every message type must provide.
/**
 * The descriptor table that encodes and decodes it, its index in that
 * table, its type name and the first eight bytes of its type hash. The
 * message headers specialize this template, so an action names types
 * and never tables.
 *
 * \tparam T the message type being described.
 */
struct msg_traits;

namespace action
{
template<typename T>
/// What every action type must provide.
/**
 * The goal, result and feedback message types the sketch's action struct
 * names. The generated action headers name them as nested types, so a
 * sketch names an action and never a part.
 *
 * \tparam T the action type being described.
 */
struct traits;

/// The goal decision the handle_goal callback returns.
/**
 * Run the goal or refuse it. There is no deferral: one active goal per
 * action leaves no queue, so a second goal is answered busy without
 * reaching the callback.
 */
enum class GoalResponse : uint8_t { REJECT = 1u, ACCEPT_AND_EXECUTE = 2u };

/// The cancel decision the handle_cancel callback returns.
/**
 * Stop the goal or let it run.
 */
enum class CancelResponse : uint8_t { REJECT = 1u, ACCEPT = 2u };

namespace detail
{
template<typename T>
/// One server's typed storage and callbacks.
/**
 * The registry keeps the record, so the upcalls stay typed while the
 * core sees only plain functions.
 *
 * \tparam T the action type this record serves.
 */
struct server_record
{
  T goal;
};
} // namespace detail

namespace detail
{
/// Registers one server's action and callbacks, binding the core table.
/**
 * Hands back the server's index, or 0xFF when the registry refuses it.
 *
 * \param[in] action_name what to announce.
 * \param[in] type_name the announced type name.
 * \param[in] type_hash the first eight bytes of the type hash.
 * \param[in] record the typed server record the dispatches receive.
 * \param[in] dispatch_goal the typed goal upcall.
 * \param[in] dispatch_cancel the typed cancel upcall.
 * \return The server's index, or 0xFF on refusal.
 */
uint8_t register_server(
  const char *action_name, const char *type_name, const uint8_t *type_hash,
  void *record,
  void (*dispatch_goal)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t),
  void (*dispatch_cancel)(void *, uint8_t, uint8_t));
} // namespace detail

template<typename T>
/// The handle to the running goal, owned by its server.
/**
 * One goal per action, so one handle per server. The application answers
 * the goal through it, from the accepted callback or later.
 *
 * \tparam T the action type this handle answers.
 */
class Server;

/* IWYU: the handle names the server that owns it. */
template<typename T>
/// The running goal this server owns, answered through its server.
/**
 * \tparam T the action type this handle answers.
 */
class ServerGoalHandle
{
public:
  /// The accepted goal, as the upcall decoded it.
  /**
   * Valid while the goal runs; the server owns the storage.
   *
   * \return The running goal message.
   */
  const typename T::Goal & get_goal() const
  {
    return *m_goal;
  }

  /// Sends this feedback for the running goal best effort.
  /**
   * Fails while the goal is not running, sending nothing.
   *
   * \param[in] msg the feedback to send.
   * \return R2S_OK, or R2S_ERR_BOUNDS when the message does not encode,
   * or the core feedback errors.
   */
  r2s_status_t publish_feedback(const typename T::Feedback & msg)
  {
    r2s_codec_out_t out = {m_encode, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;

    if (r2s_codec_encode(msg_traits<typename T::Feedback>::table(),
                             msg_traits<typename T::Feedback>::type_index(), &msg,
          R2S_ACTION_NARROW, &out,
                             &field) != R2S_OK)
    {
      return R2S_ERR_BOUNDS;
    }
    return r2s_actions_feedback(m_index, m_seq, m_encode, out.len);
  }

  /// True once an accepted cancel arrived for the running goal.
  /**
   * \return Whether the running goal is canceling.
   */
  bool is_canceling() const
  {
    return m_canceling;
  }

  /// Ends the running goal as succeeded.
  /**
   * Fails while the goal is not running, sending nothing.
   *
   * \param[in] msg the result to send.
   * \return R2S_OK, or R2S_ERR_BOUNDS when the message does not encode,
   * or the core result errors.
   */
  r2s_status_t succeed(const typename T::Result & msg)
  {
    return send_result(msg, R2S_ACT_SUCCEEDED);
  }
  /// Ends the running goal as aborted.
  /**
   * Fails while the goal is not running, sending nothing.
   *
   * \param[in] msg the result to send.
   * \return R2S_OK, or R2S_ERR_BOUNDS when the message does not encode,
   * or the core result errors.
   */
  r2s_status_t abort(const typename T::Result & msg)
  {
    return send_result(msg, R2S_ACT_ABORTED);
  }
  /// Ends the running goal as canceled after an accepted cancel.
  /**
   * Fails while the goal is not running, sending nothing.
   *
   * \param[in] msg the result to send.
   * \return R2S_OK, or R2S_ERR_BOUNDS when the message does not encode,
   * or the core result errors.
   */
  r2s_status_t canceled(const typename T::Result & msg)
  {
    return send_result(msg, R2S_ACT_CANCELED);
  }

private:
  friend class Server<T>;

  ServerGoalHandle()
  : m_goal(NULL), m_encode(NULL), m_index(0xFFu), m_seq(0u), m_canceling(false)
  {
  }

  r2s_status_t send_result(const typename T::Result & msg, uint8_t outcome)
  {
    r2s_codec_out_t out = {m_encode, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;

    if (r2s_codec_encode(msg_traits<typename T::Result>::table(),
                             msg_traits<typename T::Result>::type_index(), &msg, R2S_ACTION_NARROW,
          &out,
                             &field) != R2S_OK)
    {
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

template<typename T>
/// The MCU side of one action: a minimal action server with one active goal.
/**
 * Mirrors rclcpp_action::Server. The constructor registers the action
 * against the core table; the handle_goal callback answers each new goal,
 * handle_cancel answers each cancel, and handle_accepted receives the
 * running goal. Capture-less lambdas convert to the function pointers the
 * callbacks take. Keep the server alive while the node spins: the registry
 * keeps the address of its record.
 *
 * \tparam T the action type this server serves.
 */
class Server
{
public:
  /// Registers the action against the core table.
  /**
   * \param[in] action_name what to announce.
   * \param[in] goal_cb the callback answering each new goal.
   * \param[in] cancel_cb the callback answering each cancel.
   * \param[in] accepted_cb the callback receiving the running goal.
   */
  Server(
    const char *action_name, GoalResponse (*goal_cb)(const typename T::Goal & msg),
    CancelResponse (*cancel_cb)(const typename T::Goal & msg),
    void (*accepted_cb)(ServerGoalHandle<T> & handle))
  : m_index(0xFFu), m_goal_cb(goal_cb), m_cancel_cb(cancel_cb), m_accepted_cb(accepted_cb)
  {
    m_index = detail::register_server(action_name, traits<T>::type_name(), traits<T>::type_hash(),
          this,
                                          &dispatch_goal, &dispatch_cancel);
    m_handle.m_goal = &m_goal;
    m_handle.m_encode = m_encode;
    m_handle.m_index = m_index;
  }

  /// The registry index of this server, or 0xFF when registration failed.
  /**
   * \return The server's registry index.
   */
  uint8_t index() const
  {
    return m_index;
  }

private:
  Server(const Server &);
  Server & operator=(const Server &);

  static void dispatch_goal(
    void *record, uint8_t index, uint8_t goal_seq, const uint8_t *payload,
    uint16_t len)
  {
    Server<T> *self = static_cast<Server<T> *>(record);
    uint8_t field = 0xFFu;

    switch (r2s_codec_decode(msg_traits<typename T::Goal>::table(),
                                 msg_traits<typename T::Goal>::type_index(), payload, len,
          R2S_ACTION_NARROW,
                                 &self->m_goal, &field))
    {
      case R2S_OK:
        break;
      case R2S_ERR_BOUNDS:
        r2s_error_note(R2S_ERROR_BOUND, 8u, index, 1u);
        r2s_framer_count_refused();
        (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_REJECTED);
        return;
      default:
        r2s_framer_count_refused();
        (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_REJECTED);
        return;
    }
        /* The enumerators ride along by name, not value: the wire decision
         * for accepted is 0, while the enum reads like rclcpp. */
    GoalResponse decision = self->m_goal_cb(self->m_goal);
    uint8_t wire = (decision ==
      GoalResponse::ACCEPT_AND_EXECUTE) ? R2S_ACT_ACCEPTED : R2S_ACT_REJECTED;
    if (r2s_actions_answer_goal(index, goal_seq, wire) != R2S_OK) {
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
    uint8_t wire =
      (decision == CancelResponse::ACCEPT) ? R2S_ACT_CANCEL_ACCEPTED : R2S_ACT_CANCEL_REJECTED;

    if (r2s_actions_answer_cancel(index, goal_seq, wire) != R2S_OK) {
      return;
    }
    if (decision == CancelResponse::ACCEPT) {
      self->m_handle.m_canceling = true;
    }
  }

  uint8_t m_index;
  GoalResponse (*m_goal_cb)(const typename T::Goal & msg);
  CancelResponse (*m_cancel_cb)(const typename T::Goal & msg);
  void (*m_accepted_cb)(ServerGoalHandle<T> & handle);
  typename T::Goal m_goal;
  ServerGoalHandle<T> m_handle;
  uint8_t m_encode[R2S_MAX_FRAME];
};

namespace detail
{
/// True when the firmware asked for something the registry refused.
/**
 * A table the session would not bind, or more actions than the core
 * holds.
 *
 * \return Whether a registration failed.
 */
bool registration_error(void);
} // namespace detail

/// True when the firmware asked for something the registry refused.
/**
 * \return Whether a registration failed.
 */
inline bool registration_error()
{
  return detail::registration_error();
}

} // namespace action
} // namespace r2s

/// One static row: the server object the row declares.
/**
 * Registers like the runtime one. A sketch lists its actions as
 * R2S_ACTIONS rows, then expands them once at file scope, after the
 * traits specializations and callbacks the rows name: the expansion
 * instantiates each server, so it needs the whole row. Beside the
 * runtime declaration `Server<Type> obj(name, ...)` the row reads
 * `X(Type, obj, name, ...)`.
 *
 * \param[in] Type the action type this row declares.
 * \param[in] obj the server object this row defines.
 * \param[in] name what to announce.
 * \param[in] goal_cb the callback answering each new goal.
 * \param[in] cancel_cb the callback answering each cancel.
 * \param[in] accepted_cb the callback receiving the running goal.
 */
#define R2S_ACTION_X_SERVER(Type, obj, name, goal_cb, cancel_cb, accepted_cb) \
  ::r2s::action::Server<Type> obj{name, goal_cb, cancel_cb, accepted_cb};

#ifdef R2S_TESTING
/// Host test builds only: empties the registry.
/**
 * One binary runs many tests from the same power-up state.
 */
extern "C" void r2s_action_test_reset(void);
#endif

#endif
