"""The shared sketch body on both declaration styles: the runtime objects and
the X-macro list drive the same body, and the two firmwares print the same
bytes and callback counts.

Covers the 1-to-1 map: the static sketch differs from the runtime sketch
only in its declaration block, and everything after it behaves the same on
the wire.
"""

import subprocess
from pathlib import Path

from r2s_gen import bundle

ROOT = Path(__file__).resolve().parents[2]
CORE_INCLUDE = ROOT / "core" / "include"
CORE_SRC = ROOT / "core" / "src"
CPP_INCLUDE = ROOT / "cpp" / "include"
CPP_SRC = ROOT / "cpp" / "src"

RUNTIME_DECLS = """\
#include "ros2serial.h"
r2s::Node node("base_controller");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
void on_cmd_vel(const geometry_msgs::msg::Twist &msg);
r2s::Subscription<geometry_msgs::msg::Twist> cmd_vel_sub(node, "cmd_vel", on_cmd_vel);
"""

STATIC_DECLS = """\
#define R2S_TOPICS(X) \\
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \\
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include "ros2serial.h"
r2s::Node node("base_controller");
"""

SHARED_BODY = """\
/* The shared body: identical text under both declaration styles. */
#include <stdint.h>
#include <stdio.h>

#include <geometry_msgs/msg/twist.hpp>
#include <std_msgs/msg/float32.hpp>

static float s_linear;
static uint8_t s_calls;

static void set_motors(double linear, double angular)
{
    (void)linear;
    (void)angular;
}

static float read_battery(void)
{
    return 0.0f;
}

class StubPort
{
  public:
    void begin(unsigned long baud)
    {
        (void)baud;
    }
};
StubPort port;

std_msgs::msg::Float32 battery;

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    s_linear = msg.linear.x;
    s_calls++;
    set_motors(msg.linear.x, msg.angular.z);
}

extern uint8_t stub_tx_payload[2048];
extern uint16_t stub_tx_len;
extern uint32_t stub_cfg_count;
extern const char *stub_cfg_name;

static void print_tx(const char *tag)
{
    uint16_t i;

    printf("%s %u:", tag, stub_tx_len);
    for (i = 0u; i < stub_tx_len; i++) {
        printf(" %02x", stub_tx_payload[i]);
    }
    printf("\\n");
}

void setup()
{
    node.begin(port, 115200UL);
}

void loop()
{
    battery.data = read_battery();
    (void)battery_pub.publish(battery);
    print_tx("publish");
    node.spin_some();
}

int main(void)
{
    static uint8_t payload[256];
    r2s_codec_out_t out = {payload, (uint16_t)sizeof payload, 0u, 0u};
    uint8_t field = 0xFFu;
    geometry_msgs::msg::Twist twist = {{1.0, 0.0, 0.0}, {0.0, 0.0, 2.0}};

    setup();
    loop();
    loop();
    printf("configure %s %u\\n", stub_cfg_name, stub_cfg_count);
    if (r2s_codec_encode(r2s::msg_traits<geometry_msgs::msg::Twist>::table(),
                         r2s::msg_traits<geometry_msgs::msg::Twist>::type_index(), &twist, 0u, &out,
                         &field) != R2S_OK) {
        printf("encode failed\\n");
        return 1;
    }
    r2s_codec_dispatch(17u, 0u, payload, out.len);
    printf("calls %u linear %.1f\\n", s_calls, (double)s_linear);
    loop();
    return 0;
}
"""

STUBS = """\
/* Stubbed session and tx: both variants drive the expansion, not the link. */
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_topics.h"

uint8_t stub_tx_payload[2048];
uint16_t stub_tx_len;
static uint8_t stub_refused;
static uint8_t stub_configured;

uint8_t r2s_connected(void)
{
    return 1u;
}

uint8_t r2s_session_find_topic(uint8_t topic_id)
{
    if (stub_configured == 0u) {
        return 0xFFu;
    }
    return (uint8_t)(topic_id - 16u);
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

uint32_t stub_cfg_count;
const char *stub_cfg_name;

void r2s_init(void)
{
}

void r2s_spin_some(void)
{
}

uint32_t r2s_port_tick(void)
{
    return 0u;
}

r2s_status_t r2s_session_configure(const char *node_name, const r2s_topic_t *topics, uint8_t topic_count)
{
    (void)topics;
    stub_cfg_name = node_name;
    stub_cfg_count = topic_count;
    stub_configured = 1u;
    return R2S_OK;
}

r2s_status_t r2s_session_configure_ram(const char *node_name, const r2s_topic_t *topics, uint8_t topic_count)
{
    (void)topics;
    stub_cfg_name = node_name;
    stub_cfg_count = topic_count;
    stub_configured = 1u;
    return R2S_OK;
}
"""


def stage_bundle(tmp_path):
    """The staged bundle, as a board library stages it."""
    root = tmp_path / "bundle"
    for name, text in bundle.bundle("jazzy").items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


def bundle_limit(bundle_root, key):
    """One limit the staged bundle carries, as the Arduino build reads it."""
    text = (bundle_root / "src" / "r2s_bundled.h").read_text(encoding="utf-8")
    for line in text.splitlines():
        if line.startswith(f"#define {key} "):
            return line.split()[2]
    raise AssertionError(f"staged bundle names no {key}")


def frame_limit(bundle_root):
    """The frame limit the staged bundle carries."""
    return bundle_limit(bundle_root, "R2S_MAX_FRAME")


def nest_limit(bundle_root):
    """The nesting limit the staged bundle carries."""
    return bundle_limit(bundle_root, "R2S_MAX_NEST")


def compile_variant(tmp_path, bundle_root, decls, name, extra_sources):
    """One shared-body variant: the declarations differ, the body does not."""
    root = tmp_path / name
    root.mkdir(parents=True, exist_ok=True)
    (root / "sketch.cpp").write_text(decls + SHARED_BODY, encoding="utf-8")
    (root / "stubs.c").write_text(STUBS, encoding="utf-8")
    frame = frame_limit(bundle_root)
    nest = nest_limit(bundle_root)
    objects = []
    for source, compiler, std, extra in [
        (root / "sketch.cpp", "g++", "c++11", ()),
        (root / "stubs.c", "cc", "c99", ()),
        (bundle_root / "src" / "r2s_bundled.c", "cc", "c99", ()),
        *extra_sources,
    ]:
        obj = root / (source.stem + ".o")
        compiled = subprocess.run(
            [
                compiler,
                f"-std={std}",
                "-pedantic",
                "-Werror",
                "-Wall",
                "-Wextra",
                "-I",
                str(root),
                "-I",
                str(bundle_root / "src"),
                "-I",
                str(CORE_INCLUDE),
                "-I",
                str(CPP_INCLUDE),
                *extra,
                "-c",
                str(source),
                "-o",
                str(obj),
            ],
            capture_output=True,
            text=True,
        )
        assert compiled.returncode == 0, compiled.stderr
        objects.append(obj)
    for source in (CORE_SRC / "codec.c", CORE_SRC / "varint.c"):
        obj = root / (source.stem + ".o")
        compiled = subprocess.run(
            [
                "cc",
                "-std=c99",
                "-pedantic",
                "-Werror",
                "-Wall",
                "-Wextra",
                "-I",
                str(CORE_INCLUDE),
                "-DR2S_WITH_CODEC",
                f"-DR2S_MAX_FRAME={frame}",
                f"-DR2S_MAX_NEST={nest}",
                "-c",
                str(source),
                "-o",
                str(obj),
            ],
            capture_output=True,
            text=True,
        )
        assert compiled.returncode == 0, compiled.stderr
        objects.append(obj)
    linked = subprocess.run(
        ["g++", *[str(obj) for obj in objects], "-o", str(root / "sketch")],
        capture_output=True,
        text=True,
    )
    assert linked.returncode == 0, linked.stderr
    return root / "sketch"


def run_variant(binary):
    """The variant's printed bytes and callback counts."""
    run = subprocess.run([str(binary)], capture_output=True, text=True)

    assert run.returncode == 0, run.stdout + run.stderr
    assert "publish" in run.stdout
    return run.stdout


def test_the_shared_body_behaves_the_same_on_both_declaration_styles(tmp_path):
    bundle_root = stage_bundle(tmp_path)
    runtime_sources = [(CPP_SRC / "runtime.cpp", "g++", "c++11", ())]
    runtime = compile_variant(tmp_path, bundle_root, RUNTIME_DECLS, "runtime", runtime_sources)
    statik = compile_variant(tmp_path, bundle_root, STATIC_DECLS, "static", [])

    assert run_variant(runtime) == run_variant(statik)
