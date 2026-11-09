"""CDR ground truth for the encoding benchmark: serialize the representative
payloads with the ROS container's own rmw_serialize and print each length.

Reads {"key": {"type": name, "message": mapping}} on stdin, prints
{"key": length} on stdout. A {"bits": hex} float value carries exact bits.
Runs in the dev image, where rclpy is installed; the tests skip without it.
"""

import json
import struct
import sys


def f32(value):
    if isinstance(value, dict):
        return struct.unpack('<f', bytes.fromhex(value['bits']))[0]
    return value


def build_time(message):
    from builtin_interfaces.msg import Time

    return Time(sec=message['sec'], nanosec=message['nanosec'])


def build_vector3(message):
    from geometry_msgs.msg import Vector3

    return Vector3(x=message['x'], y=message['y'], z=message['z'])


def build_quaternion(message):
    from geometry_msgs.msg import Quaternion

    return Quaternion(x=message['x'], y=message['y'], z=message['z'], w=message['w'])


def build_header(message):
    from std_msgs.msg import Header

    return Header(stamp=build_time(message['stamp']), frame_id=message['frame_id'])


def build_transform(message):
    from geometry_msgs.msg import Transform

    return Transform(
        translation=build_vector3(message['translation']),
        rotation=build_quaternion(message['rotation']),
    )


def build_stamped(message):
    from geometry_msgs.msg import TransformStamped

    return TransformStamped(
        header=build_header(message['header']),
        child_frame_id=message['child_frame_id'],
        transform=build_transform(message['transform']),
    )


def build(type_name, message):
    if type_name == 'std_msgs/msg/Float32':
        from std_msgs.msg import Float32

        return Float32(data=f32(message['data']))
    if type_name == 'builtin_interfaces/msg/Time':
        return build_time(message)
    if type_name == 'geometry_msgs/msg/Vector3':
        return build_vector3(message)
    if type_name == 'geometry_msgs/msg/Quaternion':
        return build_quaternion(message)
    if type_name == 'geometry_msgs/msg/Twist':
        from geometry_msgs.msg import Twist

        return Twist(
            linear=build_vector3(message['linear']), angular=build_vector3(message['angular'])
        )
    if type_name == 'std_msgs/msg/Header':
        return build_header(message)
    if type_name == 'std_msgs/msg/String':
        from std_msgs.msg import String

        return String(data=message['data'])
    if type_name == 'geometry_msgs/msg/Transform':
        return build_transform(message)
    if type_name == 'geometry_msgs/msg/TransformStamped':
        return build_stamped(message)
    if type_name == 'tf2_msgs/msg/TFMessage':
        from tf2_msgs.msg import TFMessage

        return TFMessage(transforms=[build_stamped(item) for item in message['transforms']])
    if type_name == 'sensor_msgs/msg/Range':
        from sensor_msgs.msg import Range

        return Range(
            header=build_header(message['header']),
            radiation_type=message['radiation_type'],
            field_of_view=f32(message['field_of_view']),
            min_range=f32(message['min_range']),
            max_range=f32(message['max_range']),
            range=f32(message['range']),
            variance=f32(message['variance']),
        )
    if type_name == 'sensor_msgs/msg/Imu':
        from sensor_msgs.msg import Imu

        return Imu(
            header=build_header(message['header']),
            orientation=build_quaternion(message['orientation']),
            orientation_covariance=[float(v) for v in message['orientation_covariance']],
            angular_velocity=build_vector3(message['angular_velocity']),
            angular_velocity_covariance=[float(v) for v in message['angular_velocity_covariance']],
            linear_acceleration=build_vector3(message['linear_acceleration']),
            linear_acceleration_covariance=[
                float(v) for v in message['linear_acceleration_covariance']
            ],
        )
    if type_name == 'sensor_msgs/msg/JointState':
        from sensor_msgs.msg import JointState

        return JointState(
            header=build_header(message['header']),
            name=list(message['name']),
            position=[float(v) for v in message['position']],
            velocity=[float(v) for v in message['velocity']],
            effort=[float(v) for v in message['effort']],
        )
    raise ValueError(f'unknown type {type_name}')


def main():
    from rclpy.serialization import serialize_message

    cases = json.load(sys.stdin)
    out = {}
    for key, case in cases.items():
        out[key] = len(serialize_message(build(case['type'], case['message'])))
    print(json.dumps(out))


if __name__ == '__main__':
    main()
