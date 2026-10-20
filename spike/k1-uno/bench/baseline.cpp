// Baseline: the same Serial usage as the libraries under test, nothing else.
#include <Arduino.h>
void setup() { Serial.begin(115200); }
void loop() {
  int c = Serial.read();
  if (c >= 0) Serial.write((uint8_t)c);
}
