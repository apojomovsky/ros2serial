"""The `services` section: the service table the firmware announces.

A service entry names one ROS service, the role the MCU plays for it, the
request and response descriptor indices the depth-first walk fixed, and the
first 8 bytes of the service's RIHS01 hash (spec/protocol.md, 7.8).
"""

from collections.abc import Mapping
from dataclasses import dataclass

from r2s_gen import bounds
from r2s_gen import descriptors
from r2s_gen import msgs
from r2s_gen import topics
from r2s_gen import typehash
from r2s_gen.config import Config
from r2s_gen.msgs import MessageError
from r2s_gen.msgs import MessageRepository

ROLE_BYTES = {'server': 0, 'client': 1}
MAX_SERVICES = 8


@dataclass(frozen=True)
class ServiceEntry:
    name: str
    type_name: str
    role: str
    c_name: str
    service_id: int
    type_index_request: int
    type_index_response: int
    role_byte: int
    type_hash: bytes


@dataclass(frozen=True)
class ServiceTable:
    node: str
    entries: tuple[ServiceEntry, ...]


def roots(loaded: Config) -> list[str]:
    """The descriptor roots one service section needs: its requests and responses."""
    names: list[str] = []
    for service in loaded.services:
        canonical = msgs.canonical_type_name(service.type_name)
        names.extend([f'{canonical}_Request', f'{canonical}_Response'])
    return names


def build(
    loaded: Config,
    resolved: Mapping[str, bounds.ResolvedMessage],
    repository: MessageRepository,
    table: descriptors.DescriptorTable | None = None,
    source: str = 'r2s.yaml',
) -> ServiceTable:
    """The service table for `config`: IDs, roles, hashes and descriptor indices."""
    if len(loaded.services) > MAX_SERVICES:
        raise MessageError(
            f'{source}: services',
            f'{len(loaded.services)} services exceed the {MAX_SERVICES} the core tracks',
            'remove some services',
        )
    if table is None:
        table = descriptors.build(resolved, roots(loaded))
    entries = tuple(
        ServiceEntry(
            name=service.name,
            type_name=msgs.canonical_type_name(service.type_name),
            role=service.role,
            c_name=topics.c_identifier(service.name),
            service_id=position,
            type_index_request=table.index_of[
                f'{msgs.canonical_type_name(service.type_name)}_Request'
            ],
            type_index_response=table.index_of[
                f'{msgs.canonical_type_name(service.type_name)}_Response'
            ],
            role_byte=ROLE_BYTES[service.role],
            type_hash=_hash(repository, service.type_name),
        )
        for position, service in enumerate(loaded.services)
    )
    return ServiceTable(node=loaded.node, entries=entries)


def _hash(repository: MessageRepository, type_name: str) -> bytes:
    """The first 8 bytes of the service's RIHS01 hash, as SVR-7 carries them."""
    digest = typehash.service_hash(repository, type_name)
    return bytes.fromhex(digest[len('RIHS01_') :])[:8]


def emit_h(table: ServiceTable) -> str:
    """The service section of the generated header: the count, the IDs and the table."""
    lines = [
        '/* Services in configuration order: IDs run from 0, each service',
        ' * names its request and response by the descriptor indices the',
        ' * depth-first walk fixed (DSC-11), and the role byte is 0 when the',
        ' * MCU serves the service and 1 when it calls it (SVR-2). */',
        '',
        f'#define R2S_SERVICE_COUNT {len(table.entries)}u',
    ]
    if table.entries:
        lines.extend(
            [
                '#ifndef R2S_SERVICE_MAX_COUNT',
                f'#define R2S_SERVICE_MAX_COUNT {len(table.entries)}u',
                '#endif',
                '',
                '#include "r2s/r2s_services.h"',
                '',
            ]
        )
    for entry in table.entries:
        lines.extend(
            topics._define_lines(f'R2S_SERVICE_{entry.c_name.upper()}_ID', f'{entry.service_id}u')
        )
    for entry in table.entries:
        lines.extend(
            topics._define_lines(
                f'R2S_SERVICE_{entry.c_name.upper()}_ROLE', f'0x{entry.role_byte:02x}u'
            )
        )
    for entry in table.entries:
        lines.extend(
            topics._define_lines(
                f'R2S_SERVICE_{entry.c_name.upper()}_REQUEST_TYPE',
                f'{entry.type_index_request}u',
            )
        )
    for entry in table.entries:
        lines.extend(
            topics._define_lines(
                f'R2S_SERVICE_{entry.c_name.upper()}_RESPONSE_TYPE',
                f'{entry.type_index_response}u',
            )
        )
    if table.entries:
        lines.extend(
            [
                '',
                '/* The announced services and the service verdicts (SVR-7); a board',
                ' * that renames services at launch copies this table. */',
                'extern const r2s_service_t R2S_FLASH r2s_services[R2S_SERVICE_COUNT];',
                '',
            ]
        )
    return '\n'.join(lines)


def emit_c(table: ServiceTable) -> str:
    """The service section of the generated source: the announced table."""
    lines = [
        '/* The announced services (SVR-2, SVR-7): the hashes are the first 8 bytes',
        " * of each service's RIHS01 hash, and the request and response types are",
        ' * descriptor indices into the shared table. */',
        'const r2s_service_t R2S_FLASH r2s_services[R2S_SERVICE_COUNT] = {',
    ]
    for entry in table.entries:
        digest = ', '.join(f'0x{byte:02x}u' for byte in entry.type_hash)
        lines.extend(
            [
                *topics._string_row('  {', entry.type_name, ','),
                *topics._string_row('    ', entry.name, ','),
                f'    R2S_SERVICE_{entry.c_name.upper()}_ROLE,',
                f'    {{{digest}}}}},',
            ]
        )
    lines.append('};')
    lines.append('')
    return '\n'.join(lines)


def generate(
    loaded: Config,
    resolved: Mapping[str, bounds.ResolvedMessage],
    repository: MessageRepository,
    source: str = 'r2s.yaml',
) -> tuple[str, str]:
    """The firmware's service header and source fragments for `config`."""
    table = build(
        loaded, resolved, repository, descriptors.build(resolved, roots(loaded)), source=source
    )
    return emit_h(table), emit_c(table)
