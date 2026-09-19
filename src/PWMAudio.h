/*
    PWMAudio (ESP32-S3 port)
    Plays a signed 16b audio stream on a user defined pin using LEDC PWM

    Ported from arduino-pico PWMAudio:
    https://github.com/earlephilhower/arduino-pico/tree/master/libraries/PWMAudio

    Copyright (c) 2022 Earle F. Philhower, III <earlephilhower@yahoo.com>
    Copyright (c) 2026 fukuen

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, see <https://www.gnu.org/licenses/>.
*/

#pragma once
#include <Arduino.h>
#include "driver/ledc.h"

// ESP32-S3 LEDC を使った 16bit ストリーミング PWM 出力エンジン。
// コンシューマは LEDC タイマーオーバーフロー ISR で、リングバッファから
// サンプルレートに合わせてサンプルを引き出します (RP2040 の DMA 相当)。
class PWMAudio {
public:
    PWMAudio(uint8_t pin = 0, bool stereo = false);
    virtual ~PWMAudio();

    bool setBuffers(size_t buffers, size_t bufferWords);
    bool setPWMFrequency(int newFreq);
    bool setFrequency(int frequency);
    bool setPin(uint8_t pin);
    bool setStereo(bool stereo = true);
    bool setBitsPerSample(int bits) { return bits == 16; }

    bool begin(long sampleRate);
    bool end();

    size_t write(int16_t val, bool sync = true);
    int available();
    int availableForWrite();
    void flush();

    // Note that these callback are called from **INTERRUPT CONTEXT** and hence
    // should be in RAM, not FLASH, and should be quick to execute.
    void onTransmit(void(*)(void));
    void onTransmit(void(*)(void *), void *data);

    bool getUnderflow();

private:
    void find_resolution();
    static void IRAM_ATTR isr(void *arg);

    uint8_t _pin;
    bool _stereo;
    int _freq;             // PWM carrier frequency (Hz)
    int _sampleRate;       // audio sample rate (Hz)
    size_t _buffers;
    size_t _bufferWords;
    uint8_t _resolution;   // LEDC duty resolution (bits)
    uint32_t _pwmMax;      // max duty value (2^resolution - 1)
    bool _running;
    bool _wasHolding;
    uint32_t _holdWord;
    void (*_cb)();
    void (*_cbd)(void *);
    void *_cbdata;
    uint32_t _overunderflow;
    uint32_t _acc;         // sample rate accumulator
    size_t _consumerCount;

    uint32_t *_ring;       // lock-free ring buffer (32b words: L<<16|R)
    size_t _ringSize;      // words
    volatile size_t _head; // consumer (ISR) index
    volatile size_t _tail; // producer (write) index

    ledc_isr_handle_t _isrHandle;
};