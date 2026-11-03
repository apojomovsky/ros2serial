/* rosserial-compatible Subscriber: the sketch hands it a topic and a callback,
 * and subscribe() announces it on the runtime topic table. The two arities
 * follow ros_lib: a member function with its object, or a free function. */
#ifndef R2S_ROS_SUBSCRIBER_H
#define R2S_ROS_SUBSCRIBER_H

#include <stdint.h>

#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/runtime.hpp"

namespace ros
{

/* Member-function subscriber. Decodes into its own store, as ros_lib does,
 * then calls the object. No virtuals: subscribe() takes the concrete type,
 * so the layer never erases it. */
template <typename MsgT, typename ObjT = void> class Subscriber
{
  public:
    typedef void (ObjT::*CallbackT)(const MsgT &);
    MsgT msg;

    /* Keeps the topic and the callback. The endpoint rides along for
     * sketches written against ros_lib and changes nothing announced. */
    Subscriber(const char *topic, CallbackT cb, ObjT *obj, int endpoint = 0)
        : topic_(topic), id_(-1), m_callback(cb), m_object(obj), m_endpoint(endpoint)
    {
    }

    int getEndpointType()
    {
        return m_endpoint;
    }

    const char *topic_;
    /* Set by NodeHandle when it subscribes; -1 until then. */
    int id_;

    /* Decodes the frame into the subscriber's store and calls it. Passed to
     * the registry, which keeps the subscriber's address as the record, so
     * the subscriber must outlive the spinning node. */
    static void dispatch(void *record, const uint8_t *payload, uint16_t len, uint8_t narrow)
    {
        Subscriber *self = static_cast<Subscriber *>(record);
        uint8_t field = 0xFFu;

        if (r2s_codec_decode(r2s::msg_traits<MsgT>::table(), r2s::msg_traits<MsgT>::type_index(), payload,
                             len, narrow, &self->msg, &field) != R2S_OK) {
            r2s_framer_count_refused();
            return;
        }
        (self->m_object->*self->m_callback)(self->msg);
    }

  private:
    CallbackT m_callback;
    ObjT *m_object;
    int m_endpoint;
};

/* Standalone-function subscriber. A capture-less lambda converts to the
 * function pointer the constructor takes. */
template <typename MsgT> class Subscriber<MsgT, void>
{
  public:
    typedef void (*CallbackT)(const MsgT &);
    MsgT msg;

    /* Keeps the topic and the callback. The endpoint rides along for
     * sketches written against ros_lib and changes nothing announced. */
    Subscriber(const char *topic, CallbackT cb, int endpoint = 0)
        : topic_(topic), id_(-1), m_callback(cb), m_endpoint(endpoint)
    {
    }

    int getEndpointType()
    {
        return m_endpoint;
    }

    const char *topic_;
    /* Set by NodeHandle when it subscribes; -1 until then. */
    int id_;

    /* Decodes the frame into the subscriber's store and calls it. Passed to
     * the registry, which keeps the subscriber's address as the record, so
     * the subscriber must outlive the spinning node. */
    static void dispatch(void *record, const uint8_t *payload, uint16_t len, uint8_t narrow)
    {
        Subscriber *self = static_cast<Subscriber *>(record);
        uint8_t field = 0xFFu;

        if (r2s_codec_decode(r2s::msg_traits<MsgT>::table(), r2s::msg_traits<MsgT>::type_index(), payload,
                             len, narrow, &self->msg, &field) != R2S_OK) {
            r2s_framer_count_refused();
            return;
        }
        self->m_callback(self->msg);
    }

  private:
    CallbackT m_callback;
    int m_endpoint;
};

} // namespace ros

#endif
