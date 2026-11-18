# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""A custom type through the X-macro route, against the Python reference.

The library `r2s-gen types` emits is consumed by the same sketch header the
bundled library ships, so a row naming a user's type expands exactly like a
bundled one. Each host proof compiles that header with the real core codec and
a stubbed session, then checks three facts against `r2s_gen.reference`: the
announced table entry, the bytes the publish wrapper sends, and the dispatch
into the row's callback. One proof spells the row in C and one in C++, because
the two spellings resolve the type through different header paths.
"""

import re
import subprocess
from pathlib import Path

from r2s_gen import msgs
from r2s_gen import typehash
from r2s_gen import types
from r2s_gen.reference import messages

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / 'examples'
CORE_INCLUDE = ROOT / 'core' / 'include'
CORE_SRC = ROOT / 'core' / 'src'

TYPE_NAME = 'custom_interfaces/msg/SensorReading'
C_TYPE = 'custom_interfaces__msg__SensorReading'
CPP_TYPE = 'custom_interfaces::msg::SensorReading'

WARNINGS = (
    '-pedantic',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wconversion',
    '-Wshadow',
    '-Wundef',
)
# The prototype warnings exist for C only, and g++ rejects the pair outright.
C_ONLY_WARNINGS = ('-Wstrict-prototypes', '-Wmissing-prototypes')

C_SKETCH_TOPICS = f"""\
#define R2S_NODE_NAME "custom_xmacro"
#define R2S_TOPICS(X) \\
    X(Publisher, {C_TYPE}, reading_pub, "reading") \\
    X(Subscription, {C_TYPE}, reading_sub, "reading_in", on_reading)
"""

CXX_SKETCH_TOPICS = f"""\
#define R2S_NODE_NAME "custom_xmacro"
#define R2S_TOPICS(X) \\
    X(Publisher, {CPP_TYPE}, reading_pub, "reading") \\
    X(Subscription, {CPP_TYPE}, reading_sub, "reading_in", on_reading)
"""

STUBS = """\
/* Stubbed session and tx: the host proof drives the expansion, not the link.
 * The core's own headers arrive so each stub meets its prototype. */
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"

uint8_t stub_tx_payload[2048];
uint16_t stub_tx_len;
uint8_t stub_refused;
uint8_t stub_error_code;
uint8_t stub_error_topic;
uint8_t stub_error_field;
uint16_t stub_error_count;

uint8_t r2s_connected(void)
{
    return 1u;
}

uint8_t r2s_session_find_topic(uint8_t topic_id)
{
    if (topic_id != 16u && topic_id != 17u) {
        return 0xFFu;
    }
    return (uint8_t)(topic_id - 16u);
}

void r2s_error_note(uint8_t code, uint8_t topic_id, uint8_t field, uint16_t count)
{
    stub_error_code = code;
    stub_error_topic = topic_id;
    stub_error_field = field;
    stub_error_count = count;
}

uint8_t r2s_session_topic_flags(uint8_t index)
{
    (void)index;
    return 0u;
}

uint8_t r2s_session_topic_status(uint8_t index)
{
    (void)index;
    return 0u;
}

uint8_t r2s_session_tx_seq(uint8_t index)
{
    (void)index;
    return 0u;
}

void r2s_session_advance_tx_seq(uint8_t index)
{
    (void)index;
}

uint16_t r2s_session_note_rx_seq(uint8_t index, uint8_t seq)
{
    (void)index;
    (void)seq;
    return 0u;
}

r2s_status_t r2s_tx_begin(uint8_t topic_id, uint8_t seq, uint16_t payload_len)
{
    (void)topic_id;
    (void)seq;
    (void)payload_len;
    stub_tx_len = 0u;
    return R2S_OK;
}

r2s_status_t r2s_tx_byte(uint8_t byte)
{
    stub_tx_payload[stub_tx_len] = byte;
    stub_tx_len++;
    return R2S_OK;
}

r2s_status_t r2s_tx_end(void)
{
    return R2S_OK;
}

void r2s_framer_count_refused(void)
{
    stub_refused++;
}
"""


def plan():
    """The type library `r2s-gen types` resolves for the example package's message."""
    return types.plan(
        [types.Entry(TYPE_NAME)],
        distro='jazzy',
        msg_paths=[str(EXAMPLES)],
        source='the command line',
    )


def emitted():
    return types.emit(plan(), 'arduino')


def staged(tmp_path, files):
    """Write the emitted library beside the core headers, as the firmware build stages it."""
    root = tmp_path / 'sketch'
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
    (root / 'src' / 'r2s').mkdir(parents=True, exist_ok=True)
    for header in sorted((CORE_INCLUDE / 'r2s').glob('*.h')):
        (root / 'src' / 'r2s' / header.name).write_text(
            header.read_text(encoding='utf-8'), encoding='utf-8'
        )
    return root


def limit(files, name):
    match = re.search(rf'^#define {name} ([0-9]+)u$', files['src/r2s_bundled.h'], re.MULTILINE)
    assert match is not None, name
    return match.group(1)


def defines(files):
    """The flags a firmware build passes, read from the emitted limits."""
    return (
        '-DR2S_WITH_CODEC',
        f'-DR2S_MAX_FRAME={limit(files, "R2S_MAX_FRAME")}',
        f'-DR2S_MAX_NEST={limit(files, "R2S_MAX_NEST")}',
    )


def reference():
    """The payload and the first eight hash bytes every assertion below checks against."""
    repository = msgs.MessageRepository(msg_paths=[EXAMPLES], distro='jazzy')
    payload = messages.encode(
        repository, TYPE_NAME, {'temperature': 21.5, 'status': 7}, narrow=False
    )
    digest = typehash.type_hash(repository, TYPE_NAME)
    hashed = ', '.join(f'0x{byte:02x}u' for byte in bytes.fromhex(digest[len('RIHS01_') :])[:8])
    return payload, hashed


def c_bytes(payload):
    return ', '.join(f'0x{byte:02x}' for byte in payload)


CHECK = """\
#define CHECK(cond) \\
    do { \\
        if (!(cond)) { \\
            printf("fail line %d: %s\\n", __LINE__, #cond); \\
            return 1; \\
        } \\
    } while (0)
"""

STUB_EXTERNS = """\
extern uint8_t stub_tx_payload[2048];
extern uint16_t stub_tx_len;
extern uint8_t stub_refused;
extern uint16_t stub_error_count;
"""


def host_sketch_c():
    """A plain-C translation unit: the row spells the generated C struct name."""
    payload, hashed = reference()
    return (
        """\
/* The host proof of the custom type's X-macro expansion: the table, the
 * publish wrapper and dispatch against the Python reference, with stubbed
 * session and tx. The C row is the spelling a bare-metal firmware uses. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

"""
        + C_SKETCH_TOPICS
        + '#include "ros2serial.h"\n'
        + f"""
static {C_TYPE} got_reading;
static uint8_t got_calls;

"""
        + STUB_EXTERNS
        + '\n'
        + CHECK
        + f"""
void on_reading(const {C_TYPE} *msg)
{{
    got_reading = *msg;
    got_calls++;
}}

int main(void)
{{
    static const uint8_t reading_payload[] = {{ {c_bytes(payload)} }};
    static const uint8_t reading_hash[] = {{ {hashed} }};
    {C_TYPE} reading;

    CHECK(R2S_TOPIC_COUNT == 2u);
    CHECK(r2s_topics[0].topic_id == R2S_ID(reading_pub));
    /* [CTL-4] bit 0 is the direction and bit 1 the narrow flag. The host
     * build is not narrow, since its double is 64 bits, so the byte carries
     * the direction alone. */
    CHECK(r2s_topics[0].flags == 0x00u);
    CHECK(r2s_topics[1].flags == 0x01u);
    CHECK(strcmp(r2s_topics[0].type_name, "{TYPE_NAME}") == 0);
    CHECK(strcmp(r2s_topics[0].topic_name, "reading") == 0);
    CHECK(strcmp(r2s_topics[1].topic_name, "reading_in") == 0);
    CHECK(memcmp(r2s_topics[0].type_hash, reading_hash, 8) == 0);
    CHECK(memcmp(r2s_topics[1].type_hash, reading_hash, 8) == 0);

    memset(&reading, 0, sizeof reading);
    reading.temperature = 21.5f;
    reading.status = 7u;
    CHECK(r2s_publish_reading_pub(&reading) == R2S_OK);
    CHECK(stub_tx_len == sizeof reading_payload);
    CHECK(memcmp(stub_tx_payload, reading_payload, sizeof reading_payload) == 0);

    r2s_codec_dispatch(R2S_ID(reading_sub), 3u, reading_payload,
                       (uint16_t)sizeof reading_payload);
    CHECK(got_calls == 1u);
    CHECK(got_reading.temperature == 21.5f);
    CHECK(got_reading.status == 7u);
    CHECK(stub_refused == 0u);
    CHECK(stub_error_count == 0u);

    printf("custom-xmacro-c-ok\\n");
    return 0;
}}
"""
    )


def host_sketch_cxx():
    """A C++ translation unit: the row spells the double-colon type a C++ sketch uses."""
    payload, _ = reference()
    return (
        """\
/* The host proof of the custom type's X-macro expansion with the C++
 * spelling, which resolves the type through the per-package scope map. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

"""
        + CXX_SKETCH_TOPICS
        + '#include "ros2serial.h"\n'
        + f"""
static {CPP_TYPE} got_reading;
static uint8_t got_calls;

"""
        + STUB_EXTERNS
        + '\n'
        + CHECK
        + f"""
void on_reading(const {CPP_TYPE} &msg)
{{
    got_reading = msg;
    got_calls++;
}}

int main(void)
{{
    static const uint8_t reading_payload[] = {{ {c_bytes(payload)} }};
    {CPP_TYPE} reading;

    CHECK(R2S_TOPIC_COUNT == 2u);
    CHECK(strcmp(r2s_topics[0].type_name, "{TYPE_NAME}") == 0);
    CHECK(strcmp(r2s_topics[1].topic_name, "reading_in") == 0);

    memset(&reading, 0, sizeof reading);
    reading.temperature = 21.5f;
    reading.status = 7u;
    CHECK(r2s_publish_reading_pub(&reading) == R2S_OK);
    CHECK(stub_tx_len == sizeof reading_payload);
    CHECK(memcmp(stub_tx_payload, reading_payload, sizeof reading_payload) == 0);

    r2s_codec_dispatch(R2S_ID(reading_sub), 3u, reading_payload,
                       (uint16_t)sizeof reading_payload);
    CHECK(got_calls == 1u);
    CHECK(got_reading.temperature == 21.5f);
    CHECK(got_reading.status == 7u);

    printf("custom-xmacro-cxx-ok\\n");
    return 0;
}}
"""
    )


def compile_and_run(root, units, flags):
    """Compile every unit with the firmware's flags, link, and run the binary."""
    objects = []
    for source, compiler, standard in units:
        obj = root / (Path(source).stem + '.o')
        done = subprocess.run(
            [
                compiler,
                f'-std={standard}',
                *WARNINGS,
                *(C_ONLY_WARNINGS if compiler == 'cc' else ()),
                '-I',
                str(root),
                '-I',
                str(root / 'src'),
                '-I',
                str(CORE_INCLUDE),
                *flags,
                '-c',
                source,
                '-o',
                str(obj),
            ],
            capture_output=True,
            text=True,
        )
        assert done.returncode == 0, done.stderr
        objects.append(str(obj))
    linked = subprocess.run(
        ['g++', *objects, '-o', str(root / 'sketch')], capture_output=True, text=True
    )
    assert linked.returncode == 0, linked.stderr
    return subprocess.run([str(root / 'sketch')], capture_output=True, text=True)


def core_units(root):
    return [
        (str(root / 'stubs.c'), 'cc', 'c99'),
        (str(root / 'src' / 'r2s_bundled.c'), 'cc', 'c99'),
        (str(CORE_SRC / 'codec.c'), 'cc', 'c99'),
        (str(CORE_SRC / 'varint.c'), 'cc', 'c99'),
    ]


def test_the_c_expansion_matches_the_reference_on_the_host(tmp_path):
    files = emitted()
    root = staged(tmp_path, files)
    (root / 'sketch.c').write_text(host_sketch_c(), encoding='utf-8')
    (root / 'stubs.c').write_text(STUBS, encoding='utf-8')

    run = compile_and_run(
        root, [(str(root / 'sketch.c'), 'cc', 'c99'), *core_units(root)], defines(files)
    )

    assert run.returncode == 0, run.stdout + run.stderr
    assert 'custom-xmacro-c-ok' in run.stdout


def test_the_cxx_expansion_matches_the_reference_on_the_host(tmp_path):
    files = emitted()
    root = staged(tmp_path, files)
    (root / 'sketch.cpp').write_text(host_sketch_cxx(), encoding='utf-8')
    (root / 'stubs.c').write_text(STUBS, encoding='utf-8')

    run = compile_and_run(
        root, [(str(root / 'sketch.cpp'), 'g++', 'c++11'), *core_units(root)], defines(files)
    )

    assert run.returncode == 0, run.stdout + run.stderr
    assert 'custom-xmacro-cxx-ok' in run.stdout
