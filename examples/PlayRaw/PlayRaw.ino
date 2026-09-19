/*
  This example plays a raw, headerless, mono 16b, 44.1k sample using the
  ESP32-PWMAudio engine on GPIO 42.  Hook amp/speaker/buzzer between GPIO42
  and convenient GND (stereo engine uses GPIO42 and GPIO43; CH1 unconnected
  is fine for mono wiring).

  Ported from arduino-pico libraries/PWMAudio/examples/PlayRaw
*/

#include <PWMAudio.h>
#include "wav.h"

// The sample pointers
const int16_t *start = (const int16_t *)out_raw;
const int16_t *p = start;

// Create the PWM audio device on GPIO 42 (mono engine, 1 channel)
PWMAudio pwm(42);

unsigned int count = 0;

void cb() {
  while (pwm.available()) {
    pwm.write(*p++);
    count += 2;
    if (count >= sizeof(out_raw)) {
      count = 0;
      p = start;
    }
  }
}

void setup() {
  pwm.onTransmit(cb);
  pwm.begin(44100);
}

void loop() {
  /* noop, everything is done in the CB */
}