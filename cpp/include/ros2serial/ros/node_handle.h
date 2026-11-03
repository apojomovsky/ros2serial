/* rosserial-compatible NodeHandle: the sketch drives initNode, advertise,
 * subscribe and spinOnce as under ros_lib, and the layer registers the
 * topics on the runtime topic table the r2s:: API speaks. */
#ifndef R2S_ROS_NODE_HANDLE_H
#define R2S_ROS_NODE_HANDLE_H

#include <stdint.h>

#include "r2s/runtime.hpp"

#include "ros2serial/ros/publisher.h"
#include "ros2serial/ros/subscriber.h"

namespace ros
{

/* What one spinOnce() call reports. Only SPIN_OK occurs yet: the timeout
 * lands with setSpinTimeout (#417) and the stop request with services. */
const int SPIN_OK = 0;
const int SPIN_ERR = -1;
const int SPIN_TIMEOUT = -2;
const int SPIN_TX_STOP_REQUESTED = -3;
const int SPIN_TIME_RECV = -4;

/* Stands in for the hardware type a ros_lib sketch names. The layer serves
 * the port the r2s glue binds, so the type is accepted and ignored. */
struct ArduinoHardware {
};

/* The node a ported sketch drives. The template parameters mirror ros_lib
 * and are accepted and ignored: every registration runs on the runtime's
 * fixed RAM table either way. */
template <class Hardware = ArduinoHardware, int MAX_SUBSCRIBERS = 25, int MAX_PUBLISHERS = 25,
          int INPUT_SIZE = 512, int OUTPUT_SIZE = 512>
class NodeHandle_
{
  public:
    /* Owns the session's node. rosserial sketches name no node, so the
     * announce carries this one. */
    NodeHandle_() : m_node("rosserial")
    {
    }

    /* Starts the link. On Arduino this begins Serial at the stack's default
     * baud; elsewhere the port opens out of band, as the runtime firmwares
     * do, so there is nothing to start here. */
    void initNode()
    {
#ifdef ARDUINO
        m_node.begin(Serial, 115200UL);
#endif
    }

    /* Announces the publisher's topic. Returns false when the table already
     * closed with the first spin or is full, and announces nothing then. */
    bool advertise(Publisher &p)
    {
        uint8_t id = 0xFFu;

        if (m_node.register_publisher(p.topic_, p.m_table, p.m_type_index, p.m_type_name, p.m_type_hash,
                                      &id) != R2S_OK) {
            return false;
        }
        p.m_topic_id = id;
        p.id_ = (int)id;
        return true;
    }

    /* Announces the subscription. Returns false under the same refusals. */
    template <typename MsgT, typename ObjT> bool subscribe(Subscriber<MsgT, ObjT> &s)
    {
        uint8_t id = 0xFFu;

        if (m_node.register_subscriber(s.topic_, r2s::msg_traits<MsgT>::type_name(),
                                       r2s::msg_traits<MsgT>::type_hash(), &s,
                                       &Subscriber<MsgT, ObjT>::dispatch, &id) != R2S_OK) {
            return false;
        }
        s.id_ = (int)id;
        return true;
    }

    /* Serves the link once: the first call binds the table and starts the
     * handshake, the later ones move bytes and callbacks. */
    int spinOnce()
    {
        m_node.spin_some();
        return SPIN_OK;
    }

  private:
    NodeHandle_(const NodeHandle_ &);
    NodeHandle_ &operator=(const NodeHandle_ &);

    r2s::Node m_node;
};

/* The handle a ros_lib sketch names. */
typedef NodeHandle_<> NodeHandle;

} // namespace ros

#endif
