# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Size models for the encoding benchmark: packed, CDR and MAVLink-style.

The packed model follows the wire rules in spec/protocol.md ([ENC-1] to
[ENC-19]). The CDR model follows the DDS alignment rules the bridge relies
on, and the test pins it against the ROS container's own serialization. The
MAVLink-style model reorders each message's fields largest first (the wire
order MAVLink dialects use) inside a v1 or v2 envelope; its figures are a
model, not a real dialect, and the write-up says so.
"""

from __future__ import annotations

import struct

# Layout nodes: scalars ("f32", "f64", "u8", "i8", "u16", "i16", "u32",
# "i32", "u64", "i64", "bool"), strings ("str"), nested messages ("nest",
# type), fixed arrays ("fixed", element, count) and sequences ("seq",
# element). Layouts mirror the jazzy snapshots; the tests pin them there.
F32 = ('f32',)
F64 = ('f64',)
U8 = ('u8',)
I8 = ('i8',)
U16 = ('u16',)
U32 = ('u32',)
I32 = ('i32',)
U64 = ('u64',)
I64 = ('i64',)
BOOL = ('bool',)
STR = ('str',)

LAYOUTS = {
    'builtin_interfaces/msg/Time': [
        ('sec', I32),
        ('nanosec', U32),
    ],
    'geometry_msgs/msg/Vector3': [
        ('x', F64),
        ('y', F64),
        ('z', F64),
    ],
    'geometry_msgs/msg/Quaternion': [
        ('x', F64),
        ('y', F64),
        ('z', F64),
        ('w', F64),
    ],
    'geometry_msgs/msg/Twist': [
        ('linear', ('nest', 'geometry_msgs/msg/Vector3')),
        ('angular', ('nest', 'geometry_msgs/msg/Vector3')),
    ],
    'std_msgs/msg/Header': [
        ('stamp', ('nest', 'builtin_interfaces/msg/Time')),
        ('frame_id', STR),
    ],
    'std_msgs/msg/String': [
        ('data', STR),
    ],
    'std_msgs/msg/Float32': [
        ('data', F32),
    ],
    'sensor_msgs/msg/Range': [
        ('header', ('nest', 'std_msgs/msg/Header')),
        ('radiation_type', U8),
        ('field_of_view', F32),
        ('min_range', F32),
        ('max_range', F32),
        ('range', F32),
        ('variance', F32),
    ],
    'sensor_msgs/msg/Imu': [
        ('header', ('nest', 'std_msgs/msg/Header')),
        ('orientation', ('nest', 'geometry_msgs/msg/Quaternion')),
        ('orientation_covariance', ('fixed', F64, 9)),
        ('angular_velocity', ('nest', 'geometry_msgs/msg/Vector3')),
        ('angular_velocity_covariance', ('fixed', F64, 9)),
        ('linear_acceleration', ('nest', 'geometry_msgs/msg/Vector3')),
        ('linear_acceleration_covariance', ('fixed', F64, 9)),
    ],
    'sensor_msgs/msg/JointState': [
        ('header', ('nest', 'std_msgs/msg/Header')),
        ('name', ('seq', STR)),
        ('position', ('seq', F64)),
        ('velocity', ('seq', F64)),
        ('effort', ('seq', F64)),
    ],
    'std_msgs/msg/Bool': [
        ('data', BOOL),
    ],
    'geometry_msgs/msg/Transform': [
        ('translation', ('nest', 'geometry_msgs/msg/Vector3')),
        ('rotation', ('nest', 'geometry_msgs/msg/Quaternion')),
    ],
    'geometry_msgs/msg/TransformStamped': [
        ('header', ('nest', 'std_msgs/msg/Header')),
        ('child_frame_id', STR),
        ('transform', ('nest', 'geometry_msgs/msg/Transform')),
    ],
    'tf2_msgs/msg/TFMessage': [
        ('transforms', ('seq', ('nest', 'geometry_msgs/msg/TransformStamped'))),
    ],
    'builtin_interfaces/msg/Duration': [
        ('sec', I32),
        ('nanosec', U32),
    ],
    'tf2_msgs/msg/TF2Error': [
        ('error', U8),
        ('error_string', STR),
    ],
    'tf2_msgs/action/LookupTransform_Goal': [
        ('target_frame', STR),
        ('source_frame', STR),
        ('source_time', ('nest', 'builtin_interfaces/msg/Time')),
        ('timeout', ('nest', 'builtin_interfaces/msg/Duration')),
        ('target_time', ('nest', 'builtin_interfaces/msg/Time')),
        ('fixed_frame', STR),
        ('advanced', BOOL),
    ],
    'tf2_msgs/action/LookupTransform_Result': [
        ('transform', ('nest', 'geometry_msgs/msg/TransformStamped')),
        ('error', ('nest', 'tf2_msgs/msg/TF2Error')),
    ],
    'tf2_msgs/action/LookupTransform_Feedback': [],
    'std_srvs/srv/Trigger_Request': [],
    'std_srvs/srv/Trigger_Response': [
        ('success', BOOL),
        ('message', STR),
    ],
}

# Fixed sizes a MAVLink-style dialect would use, from the bundle's
# documented default bounds: strings carry their bound in bytes, numeric
# sequences their element bound, and JointState names both bounds.
MAVLINK_BOUNDS = {
    'std_msgs/msg/Header.frame_id': 16,
    'std_msgs/msg/String.data': 16,
    'geometry_msgs/msg/TransformStamped.child_frame_id': 16,
    'sensor_msgs/msg/JointState.name': [6, 16],
    'sensor_msgs/msg/JointState.position': 6,
    'sensor_msgs/msg/JointState.velocity': 6,
    'sensor_msgs/msg/JointState.effort': 6,
    'tf2_msgs/msg/TFMessage.transforms': 1,
    'std_srvs/srv/Trigger_Response.message': 16,
}

# Representative payloads, mirroring the spec vectors: the wide message and
# its narrow profile, or None where narrowing changes nothing.
REPRESENTATIVE = {
    'std_msgs/msg/Float32': {
        'wide': {'data': 1.5},
        'narrow': None,
    },
    'geometry_msgs/msg/Twist': {
        'wide': {
            'linear': {'x': 1.5, 'y': -2.0, 'z': 0.1},
            'angular': {'x': 1.0, 'y': -0.0, 'z': 3.0},
        },
        'narrow': {
            'linear': {'x': 1.5, 'y': -2.0, 'z': 0.5},
            'angular': {'x': 1.0, 'y': -0.0, 'z': 3.0},
        },
    },
    'sensor_msgs/msg/Range': {
        'wide': {
            'header': {'stamp': {'sec': 1, 'nanosec': 2}, 'frame_id': 'sonar'},
            'radiation_type': 0,
            'field_of_view': 0.5,
            'min_range': {'bits': '3ca3d70a'},
            'max_range': 4.0,
            'range': float('inf'),
            'variance': 0.0,
        },
        'narrow': {
            'header': {'stamp': {'sec': 1, 'nanosec': 2}, 'frame_id': 'sonar'},
            'radiation_type': 0,
            'field_of_view': 0.5,
            'min_range': {'bits': '3ca3d70a'},
            'max_range': 4.0,
            'range': float('inf'),
            'variance': 0.0,
        },
    },
    'sensor_msgs/msg/Imu': {
        'wide': {
            'header': {'stamp': {'sec': 100, 'nanosec': 200}, 'frame_id': 'imu0'},
            'orientation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
            'orientation_covariance': [-1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            'angular_velocity': {'x': 0.1, 'y': -0.2, 'z': 0.3},
            'angular_velocity_covariance': [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            'linear_acceleration': {'x': 0.0, 'y': 0.0, 'z': 9.81},
            'linear_acceleration_covariance': [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        },
        'narrow': {
            'header': {'stamp': {'sec': 100, 'nanosec': 200}, 'frame_id': 'imu0'},
            'orientation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
            'orientation_covariance': [-1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            'angular_velocity': {'x': 0.1, 'y': -0.2, 'z': 0.3},
            'angular_velocity_covariance': [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            'linear_acceleration': {'x': 0.0, 'y': 0.0, 'z': 9.81},
            'linear_acceleration_covariance': [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        },
    },
    'sensor_msgs/msg/JointState': {
        'wide': {
            'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'base'},
            'name': ['shoulder', 'elbow'],
            'position': [1.5, -2.5],
            'velocity': [0.0, 0.1],
            'effort': [],
        },
        'narrow': {
            'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'base'},
            'name': ['shoulder', 'elbow'],
            'position': [1.5, -2.5],
            'velocity': [0.0, 0.1],
            'effort': [],
        },
    },
    'builtin_interfaces/msg/Time': {
        'wide': {'sec': 100, 'nanosec': 200},
        'narrow': None,
    },
    'geometry_msgs/msg/Vector3': {
        'wide': {'x': 0.1, 'y': -0.2, 'z': 0.3},
        'narrow': None,
    },
    'geometry_msgs/msg/Quaternion': {
        'wide': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
        'narrow': None,
    },
    'std_msgs/msg/Header': {
        'wide': {'stamp': {'sec': 100, 'nanosec': 200}, 'frame_id': 'imu0'},
        'narrow': None,
    },
    'std_msgs/msg/String': {
        'wide': {'data': 'hello'},
        'narrow': None,
    },
    'std_msgs/msg/Bool': {
        'wide': {'data': True},
        'narrow': None,
    },
    'geometry_msgs/msg/Transform': {
        'wide': {
            'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
            'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
        },
        'narrow': {
            'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
            'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
        },
    },
    'geometry_msgs/msg/TransformStamped': {
        'wide': {
            'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'base'},
            'child_frame_id': 'gripper',
            'transform': {
                'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
                'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
            },
        },
        'narrow': {
            'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'base'},
            'child_frame_id': 'gripper',
            'transform': {
                'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
                'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
            },
        },
    },
    'tf2_msgs/msg/TFMessage': {
        'wide': {
            'transforms': [
                {
                    'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'base'},
                    'child_frame_id': 'gripper',
                    'transform': {
                        'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
                        'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
                    },
                }
            ]
        },
        'narrow': {
            'transforms': [
                {
                    'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'base'},
                    'child_frame_id': 'gripper',
                    'transform': {
                        'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
                        'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
                    },
                }
            ]
        },
    },
    'tf2_msgs/action/LookupTransform_Goal': {
        'wide': {
            'target_frame': 'odom',
            'source_frame': 'base',
            'source_time': {'sec': 7, 'nanosec': 8},
            'timeout': {'sec': 5, 'nanosec': 0},
            'target_time': {'sec': 7, 'nanosec': 8},
            'fixed_frame': '',
            'advanced': False,
        },
        'narrow': None,
    },
    'tf2_msgs/action/LookupTransform_Result': {
        'wide': {
            'transform': {
                'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'odom'},
                'child_frame_id': 'base',
                'transform': {
                    'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
                    'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
                },
            },
            'error': {'error': 0, 'error_string': ''},
        },
        'narrow': {
            'transform': {
                'header': {'stamp': {'sec': 7, 'nanosec': 8}, 'frame_id': 'odom'},
                'child_frame_id': 'base',
                'transform': {
                    'translation': {'x': 1.5, 'y': -2.0, 'z': 0.5},
                    'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
                },
            },
            'error': {'error': 0, 'error_string': ''},
        },
    },
    'tf2_msgs/action/LookupTransform_Feedback': {
        'wide': {},
        'narrow': None,
    },
    'std_srvs/srv/Trigger_Request': {
        'wide': {},
        'narrow': None,
    },
    'std_srvs/srv/Trigger_Response': {
        'wide': {'success': True, 'message': 'ok'},
        'narrow': None,
    },
}

_TOKEN = {
    'f32': 'float32',
    'f64': 'float64',
    'u8': 'uint8',
    'i8': 'int8',
    'u16': 'uint16',
    'i16': 'int16',
    'u32': 'uint32',
    'i32': 'int32',
    'u64': 'uint64',
    'i64': 'int64',
    'bool': 'bool',
    'str': 'string',
}

_STRUCT = {
    'f32': '<f',
    'f64': '<d',
    'u8': '<B',
    'i8': '<b',
    'u16': '<H',
    'i16': '<h',
    'u32': '<I',
    'i32': '<i',
    'u64': '<Q',
    'i64': '<q',
    'bool': '<B',
}

_SIZE = {
    'f32': 4,
    'f64': 8,
    'u8': 1,
    'i8': 1,
    'u16': 2,
    'i16': 2,
    'u32': 4,
    'i32': 4,
    'u64': 8,
    'i64': 8,
    'bool': 1,
}


def leaf_fields(type_name):
    """The flattened (`.msg` type token, field name) pairs in wire order."""
    found = []
    for name, node in LAYOUTS[type_name]:
        found.extend(_leaf_tokens(node, name))
    return found


def _leaf_tokens(node, name):
    kind = node[0]
    if kind == 'nest':
        return leaf_fields(node[1])
    if kind == 'fixed':
        return [(f'{_TOKEN[node[1][0]]}[{node[2]}]', name)]
    if kind == 'seq':
        if node[1][0] == 'nest':
            return leaf_fields(node[1][1])
        return [(f'{_TOKEN[node[1][0]]}[]', name)]
    return [(_TOKEN[kind], name)]


def _float_bytes(value, narrow):
    if isinstance(value, dict):
        raw = bytes.fromhex(value['bits'])
        if narrow:
            return raw[:4]
        return raw
    if narrow:
        return struct.pack('<f', value)
    return struct.pack('<d', value)


def _pack(node, value, narrow):
    kind = node[0]
    if kind == 'f32':
        if isinstance(value, dict):
            return bytes.fromhex(value['bits'])
        return struct.pack('<f', value)
    if kind == 'f64':
        return _float_bytes(value, narrow)
    if kind in _STRUCT:
        return struct.pack(_STRUCT[kind], value)
    if kind == 'str':
        raw = value.encode('utf-8')
        return _varint(len(raw)) + raw
    if kind == 'nest':
        return b''.join(_pack(field, value[name], narrow) for name, field in LAYOUTS[node[1]])
    if kind == 'fixed':
        return b''.join(_pack(node[1], item, narrow) for item in value)
    if kind == 'seq':
        return _varint(len(value)) + b''.join(_pack(node[1], item, narrow) for item in value)
    raise ValueError(f'unknown layout node {node!r}')


def _varint(count):
    out = bytearray()
    while True:
        byte = count & 0x7F
        count >>= 7
        if count:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def packed_size(type_name, message, narrow=False):
    """The packed payload length for a message mapping."""
    return sum(len(_pack(node, message[name], narrow)) for name, node in LAYOUTS[type_name])


def _cdr(node, value, parts, offset):
    kind = node[0]
    if kind in _STRUCT:
        size = _SIZE[kind]
        offset += -offset % size
        if kind == 'f32' and isinstance(value, dict):
            parts.append(bytes.fromhex(value['bits']))
        elif kind == 'f64' and isinstance(value, dict):
            parts.append(bytes.fromhex(value['bits']))
        else:
            parts.append(struct.pack(_STRUCT[kind], value))
        return offset + size
    if kind == 'str':
        offset += -offset % 4
        raw = value.encode('utf-8') + b'\x00'
        parts.append(struct.pack('<I', len(raw)))
        parts.append(raw)
        return offset + 4 + len(raw)
    if kind == 'nest':
        for name, field in LAYOUTS[node[1]]:
            offset = _cdr(field, value[name], parts, offset)
        return offset
    if kind == 'fixed':
        for item in value:
            offset = _cdr(node[1], item, parts, offset)
        return offset
    if kind == 'seq':
        offset += -offset % 4
        parts.append(struct.pack('<I', len(value)))
        offset += 4
        offset += -offset % _elem_size(node[1])
        for item in value:
            offset = _cdr(node[1], item, parts, offset)
        return offset
    raise ValueError(f'unknown layout node {node!r}')


def _align_of(type_name):
    """CDR alignment of a nested message: the widest leaf member."""
    widest = 1
    for _, node in LAYOUTS[type_name]:
        widest = max(widest, _align_node(node))
    return widest


def _align_node(node):
    kind = node[0]
    if kind == 'nest':
        return _align_of(node[1])
    if kind in ('fixed', 'seq'):
        return _align_node(node[1])
    if kind == 'str':
        return 1
    if kind in _SIZE:
        return _SIZE[kind]
    raise ValueError(f'unknown layout node {node!r}')


def _elem_size(node):
    kind = node[0]
    if kind == 'str':
        return 1
    if kind == 'nest':
        return _align_of(node[1])
    if kind in _SIZE:
        return _SIZE[kind]
    raise ValueError(f'no element size for {node!r}')


def cdr_size(type_name, message):
    """The analytic CDR length: the 4-byte header plus members aligned
    from the post-header start, which the probe pins down."""
    parts = []
    offset = 0
    for name, node in LAYOUTS[type_name]:
        offset = _cdr(node, message[name], parts, offset)
    return 4 + offset


def _mavlink_leaves(type_name, message, prefix, narrow):
    """Flattened (size, bytes) leaves with MAVLink-style fixed bounds."""
    leaves = []
    for name, node in LAYOUTS[type_name]:
        leaves.extend(_mavlink_node(type_name, name, node, message[name], narrow))
    return leaves


def _mavlink_width(type_name, narrow):
    """Fixed MAVLink-style width of one nested message from the bounds."""
    total = 0
    for name, node in LAYOUTS[type_name]:
        total += _mavlink_node_width(type_name, name, node, narrow)
    return total


def _mavlink_node_width(type_name, name, node, narrow):
    kind = node[0]
    if kind == 'f64':
        return 4 if narrow else 8
    if kind in _SIZE:
        return _SIZE[kind]
    if kind == 'str':
        return MAVLINK_BOUNDS[f'{type_name}.{name}']
    if kind == 'nest':
        return _mavlink_width(node[1], narrow)
    if kind == 'fixed':
        width = 4 if (narrow and node[1][0] == 'f64') else _SIZE[node[1][0]]
        return width * node[2]
    if kind == 'seq':
        bound = MAVLINK_BOUNDS[f'{type_name}.{name}']
        if node[1][0] == 'str':
            count, width = bound
            return count * width
        if node[1][0] == 'nest':
            return bound * _mavlink_width(node[1][1], narrow)
        width = 4 if (narrow and node[1][0] == 'f64') else _SIZE[node[1][0]]
        return bound * width
    raise ValueError(f'unknown layout node {node!r}')


def _mavlink_node(type_name, name, node, value, narrow):
    kind = node[0]
    if kind == 'f32':
        if isinstance(value, dict):
            return [(4, bytes.fromhex(value['bits']))]
        return [(4, struct.pack('<f', value))]
    if kind == 'f64':
        return [(4 if narrow else 8, _float_bytes(value, narrow))]
    if kind in _STRUCT:
        size = _SIZE[kind]
        return [(size, struct.pack(_STRUCT[kind], value))]
    if kind == 'str':
        bound = MAVLINK_BOUNDS[f'{type_name}.{name}']
        raw = value.encode('utf-8')
        if len(raw) > bound:
            raise ValueError(f'{type_name}.{name} exceeds its MAVLink bound')
        return [(bound, raw + bytes(bound - len(raw)))]
    if kind == 'nest':
        return _mavlink_leaves(node[1], value, name, narrow)
    if kind == 'fixed':
        width = 4 if (narrow and node[1][0] == 'f64') else _SIZE[node[1][0]]
        return [(width * node[2], _pack(node, value, narrow))]
    if kind == 'seq':
        bound = MAVLINK_BOUNDS[f'{type_name}.{name}']
        if node[1][0] == 'str':
            count, width = bound
            if len(value) > count:
                raise ValueError(f'{type_name}.{name} exceeds its MAVLink bound')
            slots = [
                slot.encode('utf-8') + bytes(width - len(slot.encode('utf-8'))) for slot in value
            ]
            slots += [bytes(width)] * (count - len(value))
            return [(count * width, b''.join(slots))]
        if node[1][0] == 'nest':
            elem = node[1][1]
            width = _mavlink_width(elem, narrow)
            if len(value) > bound:
                raise ValueError(f'{type_name}.{name} exceeds its MAVLink bound')
            parts = []
            for item in value:
                leaves = _mavlink_leaves(elem, item, name, narrow)
                leaves.sort(key=lambda leaf: -leaf[0])
                parts.append(b''.join(chunk for _, chunk in leaves))
            raw = b''.join(parts)
            return [(bound * width, raw + bytes(bound * width - len(raw)))]
        if len(value) > bound:
            raise ValueError(f'{type_name}.{name} exceeds its MAVLink bound')
        width = 4 if (narrow and node[1][0] == 'f64') else _SIZE[node[1][0]]
        raw = _pack(node, value, narrow)
        return [(bound * width, raw + bytes(bound * width - len(raw)))]
    raise ValueError(f'unknown layout node {node!r}')


def mavlink_size(type_name, message, version, narrow=False):
    """The MAVLink-style frame length: reordered fields in a v1/v2 envelope."""
    if version not in (1, 2):
        raise ValueError(f'unknown MAVLink version {version}')
    leaves = _mavlink_leaves(type_name, message, type_name, narrow)
    leaves.sort(key=lambda leaf: -leaf[0])
    payload = b''.join(raw for _, raw in leaves)
    if version == 1:
        return 8 + len(payload)
    return 12 + len(payload.rstrip(b'\x00'))


def narrow_breakdown(type_name, message):
    """Per-field (group, wide bytes, narrow bytes) of the float64 content."""
    groups = []
    for name, node in LAYOUTS[type_name]:
        wide, narrow = _f64_bytes(node, message[name])
        if wide:
            groups.append((name, wide, narrow))
    return groups


def _f64_bytes(node, value):
    kind = node[0]
    if kind == 'f64':
        if isinstance(value, dict):
            return (8, 4)
        return (8, 4)
    if kind == 'nest':
        wide = sum(_f64_bytes(field, value[name])[0] for name, field in LAYOUTS[node[1]])
        narrow = sum(_f64_bytes(field, value[name])[1] for name, field in LAYOUTS[node[1]])
        return (wide, narrow)
    if kind == 'fixed' and node[1] == F64:
        return (8 * len(value), 4 * len(value))
    if kind == 'seq' and node[1] == F64:
        return (8 * len(value), 4 * len(value))
    if kind in ('fixed', 'seq'):
        wide = sum(_f64_bytes(node[1], item)[0] for item in value)
        narrow = sum(_f64_bytes(node[1], item)[1] for item in value)
        return (wide, narrow)
    return (0, 0)


def table_rows():
    """One (type, profile, packed, cdr, mavlink v1, mavlink v2) row per case."""
    rows = []
    for type_name, profiles in REPRESENTATIVE.items():
        for profile in ('wide', 'narrow'):
            message = profiles[profile]
            if message is None:
                continue
            narrow = profile == 'narrow'
            rows.append(
                (
                    type_name,
                    profile,
                    packed_size(type_name, message, narrow),
                    cdr_size(type_name, message),
                    mavlink_size(type_name, message, 1, narrow),
                    mavlink_size(type_name, message, 2, narrow),
                )
            )
    return rows


def render_table():
    """The comparison table the write-up embeds verbatim."""
    lines = [
        '| Message | Profile | Packed | CDR | MAVLink v1 | MAVLink v2 |',
        '| --- | --- | --- | --- | --- | --- |',
    ]
    for type_name, profile, packed, cdr, v1, v2 in table_rows():
        lines.append(f'| `{type_name}` | {profile} | {packed} | {cdr} | {v1} | {v2} |')
    lines.append('')
    lines.append('MAVLink figures are a model, not a real dialect: fields reordered')
    lines.append("largest first with the bundle's bounds, in a v1 or v2 envelope.")
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    print(render_table(), end='')
