# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Drive the real button/LED sketch with explicit clock, GPIO and publisher inputs."""

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

STUB = r"""
#include <stdint.h>
#include <assert.h>
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define OUTPUT 1
#define R2S_OK 0
unsigned long clock_ms = 0;
int input = HIGH, Serial = 0;
int modes[4], mode_count = 0, written_pin = -1, written_value = -1;
unsigned spins = 0, sent = 0;
bool online = false, busy = false;
uint8_t published = 0;
unsigned long millis() { return clock_ms; }
int digitalRead(int) { return input; }
void digitalWrite(int pin, int value) { written_pin = pin; written_value = value; }
void pinMode(int pin, int mode) {
    (void)mode;
    if (mode_count < 4) { modes[mode_count++] = pin; }
}
namespace std_msgs { namespace msg { struct Bool { uint8_t data; }; } }
namespace r2s {
class Node {
public:
    Node(const char *) {}
    void begin(int, unsigned long) {}
    bool connected() { return online; }
    void spin_some() { ++spins; }
};
template<class T> class Publisher {
public:
    Publisher(Node &, const char *) {}
    int publish(const T &msg) {
        if (busy) { return 1; }
        published = msg.data;
        ++sent;
        return R2S_OK;
    }
};
void (*led_callback)(const std_msgs::msg::Bool &);
template<class T> class Subscription {
public:
    Subscription(Node &, const char *, void (*callback)(const T &)) { led_callback = callback; }
};
}
"""


def _drive_sketch(tmp_path, body, defines=(), led=True):
    """Compile the real sketch against the stub core and run body in main."""
    header = tmp_path / 'ros2serial.h'
    header.write_text(('#define LED_BUILTIN 13\n' if led else '') + STUB)
    include = tmp_path / 'std_msgs/msg/bool.hpp'
    include.parent.mkdir(parents=True)
    include.write_text('')
    source = tmp_path / 'test.cpp'
    source.write_text(
        '#include "'
        + str(ROOT / 'examples/arduino_button_led/arduino_button_led.ino')
        + '"\n'
        + body
    )
    program = tmp_path / 'test'
    compiled = subprocess.run(
        ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(tmp_path)]
        + ['-D' + define for define in defines]
        + [str(source), '-o', str(program)],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(program)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize(
    'define,expected',
    [
        ('CONFIG_IDF_TARGET_ESP32', 0),
        ('CONFIG_IDF_TARGET_ESP32S3', 0),
        ('CONFIG_IDF_TARGET_ESP32C3', 9),
        ('CONFIG_IDF_TARGET_ESP32C6', 9),
        ('', 2),
    ],
)
def test_button_led_reads_the_boot_button_of_each_family(tmp_path, define, expected):
    """Every ESP32 devkit has an active-low BOOT button; AVR stays on pin 2."""
    body = f"""
int main() {{
    assert(BUTTON_PIN == {expected});
    setup();
    assert(mode_count == 2);
    assert(modes[0] == {expected} && modes[1] == 13);
    r2s::led_callback(std_msgs::msg::Bool{{1}});
    assert(written_pin == 13 && written_value == HIGH);
}}
"""
    _drive_sketch(tmp_path, body, defines=(define,) if define else ())


def test_button_led_led_falls_back_to_gpio2_without_a_builtin(tmp_path):
    """The classic ESP32 variant names no LED_BUILTIN; the DevKit LED is on 2."""
    body = """
int main() {
    assert(LED_BUILTIN == 2);
    setup();
    assert(mode_count == 2 && modes[1] == 2);
    r2s::led_callback(std_msgs::msg::Bool{1});
    assert(written_pin == 2 && written_value == HIGH);
}
"""
    _drive_sketch(tmp_path, body, led=False)


def test_button_led_debounces_repeats_retries_and_drives_output(tmp_path):
    """A press settles for 20 ms, repeats every 100 ms and retries a busy port."""
    body = r"""
int main() {
    setup();
    clock_ms = 100; loop(); assert(sent == 0 && spins == 1);
    online = true; loop(); assert(sent == 1 && published == 0);
    input = LOW; clock_ms = 190; loop();
    clock_ms = 200; loop(); assert(sent == 2 && published == 0);
    clock_ms = 300; loop(); assert(sent == 3 && published == 1);
    clock_ms = 400; loop(); assert(sent == 4 && published == 1);
    busy = true; clock_ms = 500; loop(); assert(sent == 4);
    busy = false; clock_ms = 501; loop(); assert(sent == 5);
    online = false; clock_ms = 700; loop(); assert(sent == 5);
    online = true; loop(); assert(sent == 6);
    r2s::led_callback(std_msgs::msg::Bool{1}); assert(written_value == HIGH);
    r2s::led_callback(std_msgs::msg::Bool{0}); assert(written_value == LOW);
}
"""
    _drive_sketch(tmp_path, body)
