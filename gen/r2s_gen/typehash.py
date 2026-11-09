"""RIHS01 type hashes over resolved message definitions.

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
    """The `RIHS01_` hash of `type_name`, equal to the installed type support's hash."""
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
    """The synthetic `Name_Event` message the service hash covers, as rosidl generates it."""
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
    """The `service_msgs/msg/ServiceEventInfo` message, fixed by the service event design."""
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


def service_hash(repository: msgs.MessageRepository, type_name: str) -> str:
    """The `RIHS01_` hash of a `package/srv/Name`, equal to the installed service hash.

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
    """The type description with the transitively referenced types, sorted by name."""
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
