# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Drive the real button/LED sketch with explicit clock, GPIO and publisher inputs."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_button_led_debounces_repeats_retries_and_drives_output(tmp_path):
    header = tmp_path / 'ros2serial.h'
    header.write_text(r"""
#include <stdint.h>
#include <assert.h>
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define OUTPUT 1
#define LED_BUILTIN 13
#define R2S_OK 0
unsigned long clock_ms = 0;
int input = HIGH, output = LOW, Serial = 0;
unsigned spins = 0, sent = 0;
bool online = false, busy = false;
uint8_t published = 0;
unsigned long millis() { return clock_ms; }
int digitalRead(int) { return input; }
void digitalWrite(int pin, int value) { assert(pin == LED_BUILTIN); output = value; }
void pinMode(int, int) {}
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
""")
    include = tmp_path / 'std_msgs/msg/bool.hpp'
    include.parent.mkdir(parents=True)
    include.write_text('')
    source = tmp_path / 'test.cpp'
    source.write_text(
        '#include "'
        + str(ROOT / 'examples/arduino_button_led/arduino_button_led.ino')
        + '"\n'
        + r"""
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
    r2s::led_callback(std_msgs::msg::Bool{1}); assert(output == HIGH);
    r2s::led_callback(std_msgs::msg::Bool{0}); assert(output == LOW);
}
"""
    )
    program = tmp_path / 'test'
    compiled = subprocess.run(
        [
            'g++',
            '-std=c++11',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-I',
            str(tmp_path),
            str(source),
            '-o',
            str(program),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(program)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
