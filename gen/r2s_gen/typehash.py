# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
RIHS01 type hashes over resolved message definitions.

The hash is the REP-2011 RIHS01 string the installed type support reports.
The .msg to FieldType mapping follows `rosidl_adapter` (`MSG_TYPE_TO_IDL`)
and `rosidl_generator_type_description` (`FIELD_VALUE_TYPE_NAMES`), and the
canonical JSON form and sha256 step follow the latter's hash function (both
Apache 2.0, Open Source Robotics Foundation). Only the standard library is
used, so no ROS installation is needed.
"""

from __future__ import annotations

import hashlib
import json

from r2s_gen import msgs

_NESTED_TYPE_ID = 1
_BOUNDED_STRING_ID = 21
_ARRAY_OFFSET = 48
_BOUNDED_SEQUENCE_OFFSET = 96
_UNBOUNDED_SEQUENCE_OFFSET = 144

_PRIMITIVE_TYPE_IDS = {
    'bool': 15,
    'byte': 16,
    'char': 3,
    'int8': 2,
    'uint8': 3,
    'int16': 4,
    'uint16': 5,
    'int32': 6,
    'uint32': 7,
    'int64': 8,
    'uint64': 9,
    'float32': 10,
    'float64': 11,
    'string': 17,
}


def type_hash(repository: msgs.MessageRepository, type_name: str) -> str:
    """
    Compute the `RIHS01_` hash of `type_name`, as the installed type support reports.

    :param repository: the messages to resolve from.
    :param type_name: the type to hash.
    :return: the `RIHS01_` hash string.
    """
    canonical = msgs.canonical_type_name(type_name)
    descriptions: dict[str, dict] = {}
    queue = [canonical]
    while queue:
        name = queue.pop()
        if name in descriptions:
            continue
        message = repository.load(name)
        descriptions[name] = _individual_description(message)
        queue.extend(
            field['type']['nested_type_name']
            for field in descriptions[name]['fields']
            if field['type']['nested_type_name']
        )
    full = _full_description(canonical, descriptions)
    digest = hashlib.sha256(json.dumps(full, separators=(', ', ': ')).encode('utf-8')).hexdigest()
    return f'RIHS01_{digest}'


def _event_description(service: msgs.Service) -> dict:
    """
    Return the synthetic `Name_Event` message the service hash covers.

    Matches what rosidl generates.
    """
    return {
        'type_name': f'{service.type_name}_Event',
        'fields': [
            {
                'name': 'info',
                'type': {
                    'type_id': _NESTED_TYPE_ID,
                    'capacity': 0,
                    'string_capacity': 0,
                    'nested_type_name': 'service_msgs/msg/ServiceEventInfo',
                },
            },
            {
                'name': 'request',
                'type': {
                    'type_id': _BOUNDED_SEQUENCE_OFFSET + _NESTED_TYPE_ID,
                    'capacity': 1,
                    'string_capacity': 0,
                    'nested_type_name': service.request.type_name,
                },
            },
            {
                'name': 'response',
                'type': {
                    'type_id': _BOUNDED_SEQUENCE_OFFSET + _NESTED_TYPE_ID,
                    'capacity': 1,
                    'string_capacity': 0,
                    'nested_type_name': service.response.type_name,
                },
            },
        ],
    }


def _event_info_description() -> dict:
    """
    Return the `service_msgs/msg/ServiceEventInfo` message.

    Fixed by the service event design.
    """
    uint8 = {'type_id': 3, 'capacity': 0, 'string_capacity': 0, 'nested_type_name': ''}
    return {
        'type_name': 'service_msgs/msg/ServiceEventInfo',
        'fields': [
            {'name': 'event_type', 'type': dict(uint8)},
            {
                'name': 'stamp',
                'type': {
                    'type_id': _NESTED_TYPE_ID,
                    'capacity': 0,
                    'string_capacity': 0,
                    'nested_type_name': 'builtin_interfaces/msg/Time',
                },
            },
            {
                'name': 'client_gid',
                'type': {
                    'type_id': _ARRAY_OFFSET + 3,
                    'capacity': 16,
                    'string_capacity': 0,
                    'nested_type_name': '',
                },
            },
            {
                'name': 'sequence_number',
                'type': {
                    'type_id': 8,
                    'capacity': 0,
                    'string_capacity': 0,
                    'nested_type_name': '',
                },
            },
        ],
    }


def _nested(name: str) -> dict:
    """Return the field type of a nested message field."""
    return {
        'type_id': _NESTED_TYPE_ID,
        'capacity': 0,
        'string_capacity': 0,
        'nested_type_name': name,
    }


def _bounded_nested(name: str) -> dict:
    """Return the field type of a nested message sequence bound to one element."""
    return {
        'type_id': _BOUNDED_SEQUENCE_OFFSET + _NESTED_TYPE_ID,
        'capacity': 1,
        'string_capacity': 0,
        'nested_type_name': name,
    }


def _uuid_description() -> dict:
    """Return the `unique_identifier_msgs/msg/UUID` message, fixed by its package design."""
    return {
        'type_name': 'unique_identifier_msgs/msg/UUID',
        'fields': [
            {
                'name': 'uuid',
                'type': {
                    'type_id': _ARRAY_OFFSET + _PRIMITIVE_TYPE_IDS['uint8'],
                    'capacity': 16,
                    'string_capacity': 0,
                    'nested_type_name': '',
                },
            },
        ],
    }


def action_hash(repository: msgs.MessageRepository, type_name: str) -> str:
    """Return the `RIHS01_` hash of a `package/action/Name`, equal to the installed action hash.

    The action description names its goal, result and feedback messages, the
    synthesized send-goal and get-result services and the feedback message;
    the goal ID, the event info and every nested message but the goal, the
    result and the feedback load from fixed descriptions or the repository
    like any other message.
    """
    action = repository.load_action(type_name)
    goal_name = action.goal.type_name
    result_name = action.result.type_name
    feedback_name = action.feedback.type_name
    send_goal = f'{action.type_name}_SendGoal'
    get_result = f'{action.type_name}_GetResult'
    root = {
        'type_name': action.type_name,
        'fields': [
            {'name': name, 'type': _nested(nested)}
            for name, nested in (
                ('goal', goal_name),
                ('result', result_name),
                ('feedback', feedback_name),
                ('send_goal_service', send_goal),
                ('get_result_service', get_result),
                ('feedback_message', f'{action.type_name}_FeedbackMessage'),
            )
        ],
    }
    send_goal_root = {
        'type_name': send_goal,
        'fields': [
            {'name': 'request_message', 'type': _nested(f'{send_goal}_Request')},
            {'name': 'response_message', 'type': _nested(f'{send_goal}_Response')},
            {'name': 'event_message', 'type': _nested(f'{send_goal}_Event')},
        ],
    }
    send_goal_request = {
        'type_name': f'{send_goal}_Request',
        'fields': [
            {'name': 'goal_id', 'type': _nested('unique_identifier_msgs/msg/UUID')},
            {'name': 'goal', 'type': _nested(goal_name)},
        ],
    }
    send_goal_response = {
        'type_name': f'{send_goal}_Response',
        'fields': [
            {
                'name': 'accepted',
                'type': {
                    'type_id': _PRIMITIVE_TYPE_IDS['bool'],
                    'capacity': 0,
                    'string_capacity': 0,
                    'nested_type_name': '',
                },
            },
            {'name': 'stamp', 'type': _nested('builtin_interfaces/msg/Time')},
        ],
    }
    get_result_root = {
        'type_name': get_result,
        'fields': [
            {'name': 'request_message', 'type': _nested(f'{get_result}_Request')},
            {'name': 'response_message', 'type': _nested(f'{get_result}_Response')},
            {'name': 'event_message', 'type': _nested(f'{get_result}_Event')},
        ],
    }
    get_result_request = {
        'type_name': f'{get_result}_Request',
        'fields': [
            {'name': 'goal_id', 'type': _nested('unique_identifier_msgs/msg/UUID')},
        ],
    }
    get_result_response = {
        'type_name': f'{get_result}_Response',
        'fields': [
            {
                'name': 'status',
                'type': {
                    'type_id': _PRIMITIVE_TYPE_IDS['int8'],
                    'capacity': 0,
                    'string_capacity': 0,
                    'nested_type_name': '',
                },
            },
            {'name': 'result', 'type': _nested(result_name)},
        ],
    }
    descriptions: dict[str, dict] = {
        action.type_name: root,
        goal_name: _individual_description(action.goal),
        result_name: _individual_description(action.result),
        feedback_name: _individual_description(action.feedback),
        send_goal: send_goal_root,
        f'{send_goal}_Request': send_goal_request,
        f'{send_goal}_Response': send_goal_response,
        f'{send_goal}_Event': _goal_event(send_goal),
        get_result: get_result_root,
        f'{get_result}_Request': get_result_request,
        f'{get_result}_Response': get_result_response,
        f'{get_result}_Event': _goal_event(get_result),
        f'{action.type_name}_FeedbackMessage': {
            'type_name': f'{action.type_name}_FeedbackMessage',
            'fields': [
                {'name': 'goal_id', 'type': _nested('unique_identifier_msgs/msg/UUID')},
                {'name': 'feedback', 'type': _nested(feedback_name)},
            ],
        },
        'unique_identifier_msgs/msg/UUID': _uuid_description(),
        'service_msgs/msg/ServiceEventInfo': _event_info_description(),
        'builtin_interfaces/msg/Time': _individual_description(
            repository.load('builtin_interfaces/msg/Time')
        ),
    }
    queue = [
        field['type']['nested_type_name']
        for name in (goal_name, result_name, feedback_name)
        for field in descriptions[name]['fields']
        if field['type']['nested_type_name']
    ]
    while queue:
        name = queue.pop()
        if name in descriptions:
            continue
        message = repository.load(name)
        descriptions[name] = _individual_description(message)
        queue.extend(
            field['type']['nested_type_name']
            for field in descriptions[name]['fields']
            if field['type']['nested_type_name']
        )
    full = _full_description(action.type_name, descriptions)
    digest = hashlib.sha256(json.dumps(full, separators=(', ', ': ')).encode('utf-8')).hexdigest()
    return f'RIHS01_{digest}'


def _goal_event(service_name: str) -> dict:
    """Return the event description of one goal service, fixed by the service event design."""
    return {
        'type_name': f'{service_name}_Event',
        'fields': [
            {'name': 'info', 'type': _nested('service_msgs/msg/ServiceEventInfo')},
            {'name': 'request', 'type': _bounded_nested(f'{service_name}_Request')},
            {'name': 'response', 'type': _bounded_nested(f'{service_name}_Response')},
        ],
    }


def service_hash(repository: msgs.MessageRepository, type_name: str) -> str:
    """Return the `RIHS01_` hash of a `package/srv/Name`, equal to the installed service hash.

    The service description names its request, response and synthetic event
    messages; the event and its info are fixed by the service event design,
    while the request, the response and every nested message load from the
    repository like any other message.
    """
    service = repository.load_service(type_name)
    root = {
        'type_name': service.type_name,
        'fields': [
            {
                'name': f'{lower}_message',
                'type': {
                    'type_id': _NESTED_TYPE_ID,
                    'capacity': 0,
                    'string_capacity': 0,
                    'nested_type_name': f'{service.type_name}_{upper}',
                },
            }
            for lower, upper in (
                ('request', 'Request'),
                ('response', 'Response'),
                ('event', 'Event'),
            )
        ],
    }
    descriptions: dict[str, dict] = {
        service.type_name: root,
        service.request.type_name: _individual_description(service.request),
        service.response.type_name: _individual_description(service.response),
        f'{service.type_name}_Event': _event_description(service),
        'service_msgs/msg/ServiceEventInfo': _event_info_description(),
        'builtin_interfaces/msg/Time': _individual_description(
            repository.load('builtin_interfaces/msg/Time')
        ),
    }
    queue = [
        field['type']['nested_type_name']
        for name in (
            service.request.type_name,
            service.response.type_name,
        )
        for field in descriptions[name]['fields']
        if field['type']['nested_type_name']
    ]
    while queue:
        name = queue.pop()
        if name in descriptions:
            continue
        message = repository.load(name)
        descriptions[name] = _individual_description(message)
        queue.extend(
            field['type']['nested_type_name']
            for field in descriptions[name]['fields']
            if field['type']['nested_type_name']
        )
    full = _full_description(service.type_name, descriptions)
    digest = hashlib.sha256(json.dumps(full, separators=(', ', ': ')).encode('utf-8')).hexdigest()
    return f'RIHS01_{digest}'


def _individual_description(message: msgs.Message) -> dict:
    fields = message.fields
    if not fields:
        # An empty .msg hashes as one uint8 member: the adapter's IDL template renders it so.
        fields = (msgs.Field(name='structure_needs_at_least_one_member', type_name='uint8'),)
    return {
        'type_name': message.type_name,
        'fields': [
            {'name': field.name, 'type': _field_type(field, message.type_name)} for field in fields
        ],
    }


def _field_type(field: msgs.Field, where: str) -> dict:
    if field.type_name in msgs.WIDE_TYPES:
        raise msgs.MessageError(
            where, f"'{field.name}' is a wide string", 'use string or a bounded string'
        )
    if field.type_name in _PRIMITIVE_TYPE_IDS:
        base_id = _PRIMITIVE_TYPE_IDS[field.type_name]
        nested_name = ''
    else:
        base_id = _NESTED_TYPE_ID
        nested_name = field.type_name
    if field.type_name == 'string' and field.string_bound is not None:
        base_id = _BOUNDED_STRING_ID
    if field.array_length is not None:
        type_id = base_id + _ARRAY_OFFSET
        capacity = field.array_length
    elif field.array_bound is not None:
        type_id = base_id + _BOUNDED_SEQUENCE_OFFSET
        capacity = field.array_bound
    elif field.is_array:
        type_id = base_id + _UNBOUNDED_SEQUENCE_OFFSET
        capacity = 0
    else:
        type_id = base_id
        capacity = 0
    return {
        'type_id': type_id,
        'capacity': capacity,
        'string_capacity': field.string_bound or 0,
        'nested_type_name': nested_name,
    }


def _full_description(type_name: str, descriptions: dict[str, dict]) -> dict:
    """Assemble the type description with its referenced types, sorted by name."""
    references: set[str] = set()
    queue = [
        field['type']['nested_type_name']
        for field in descriptions[type_name]['fields']
        if field['type']['nested_type_name']
    ]
    while queue:
        reference = queue.pop()
        if reference in references:
            continue
        references.add(reference)
        queue.extend(
            field['type']['nested_type_name']
            for field in descriptions[reference]['fields']
            if field['type']['nested_type_name']
        )
    return {
        'type_description': descriptions[type_name],
        'referenced_type_descriptions': [descriptions[name] for name in sorted(references)],
    }
