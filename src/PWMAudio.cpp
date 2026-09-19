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
#include <Arduino.h>
#include "PWMAudio.h"
#include "esp_intr_alloc.h"
#include "soc/ledc_struct.h"
#include "soc/ledc_reg.h"

// 使用するLEDCリソース
static const ledc_mode_t    speed_mode = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t   timer_num  = LEDC_TIMER_0;
static const ledc_channel_t chL        = LEDC_CHANNEL_0;
static const ledc_channel_t chR        = LEDC_CHANNEL_1;
// LEDC の基準クロック (APB)
static const uint32_t       apbClkHz   = 80000000;

PWMAudio::PWMAudio(uint8_t pin, bool stereo) {
    _running = false;
    _pin = pin;
    _freq = 48000;
    _sampleRate = 48000;
    _buffers = 8;
    _bufferWords = 64;
    _stereo = stereo;
    _resolution = 0;
    _pwmMax = 0;
    _wasHolding = false;
    _holdWord = 0;
    _cb = nullptr;
    _cbd = nullptr;
    _cbdata = nullptr;
    _overunderflow = false;
    _acc = 0;
    _consumerCount = 0;
    _ring = nullptr;
    _ringSize = 0;
    _head = 0;
    _tail = 0;
    _isrHandle = nullptr;
}

PWMAudio::~PWMAudio() {
    end();
}

bool PWMAudio::setBuffers(size_t buffers, size_t bufferWords) {
    if (_running || (buffers < 3) || (bufferWords < 8)) {
        return false;
    }
    _buffers = buffers;
    _bufferWords = bufferWords;
    return true;
}

bool PWMAudio::setPin(uint8_t pin) {
    if (_running) {
        return false;
    }
    _pin = pin;
    return true;
}

bool PWMAudio::setStereo(bool stereo) {
    if (_running) {
        return false;
    }
    _stereo = stereo;
    return true;
}

void PWMAudio::find_resolution() {
    // キャリア周波数から LEDC 分解能を決定 (APB 80MHz / 周波数 が 1周期のカウント数)
    uint32_t div = apbClkHz / (uint32_t)_freq;
    uint32_t r = 14; // ESP32-S3 の LEDC は最大14bit
    while (((1UL << r) > div) && (r > 1)) {
        r--;
    }
    _resolution = r;
    _pwmMax = (1UL << r) - 1;
}

bool PWMAudio::setPWMFrequency(int newFreq) {
    _freq = newFreq;
    if (_running) {
        find_resolution();
        ledc_timer_config_t timer_conf = {
            .speed_mode      = speed_mode,
            .duty_resolution = (ledc_timer_bit_t)_resolution,
            .timer_num       = timer_num,
            .freq_hz         = _freq,
            .clk_cfg         = LEDC_USE_APB_CLK,
        };
        ledc_timer_config(&timer_conf);
    }
    return true;
}

bool PWMAudio::setFrequency(int frequency) {
    if (frequency == _sampleRate) {
        return true;
    }
    // レート累算器方式のため、動作中でもサンプルレートの変更は即座に反映される
    _sampleRate = frequency;
    return true;
}

bool PWMAudio::begin(long sampleRate) {
    if (_running) {
        return false;
    }
    _sampleRate = sampleRate;
    _running = true;
    _wasHolding = false;
    _acc = 0;
    _consumerCount = 0;
    _head = 0;
    _tail = 0;
    _overunderflow = false;

    find_resolution();

    // LEDC タイマー初期化 (キャリア = _freq Hz)
    ledc_timer_config_t timer_conf = {
        .speed_mode      = speed_mode,
        .duty_resolution = (ledc_timer_bit_t)_resolution,
        .timer_num       = timer_num,
        .freq_hz         = _freq,
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    if (ledc_timer_config(&timer_conf) != ESP_OK) {
        _running = false;
        return false;
    }

    // チャネル初期化 (CH0 = _pin、ステレオ時 CH1 = _pin + 1)
    ledc_channel_config_t chConf = {
        .gpio_num   = _pin,
        .speed_mode = speed_mode,
        .channel    = chL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = timer_num,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&chConf) != ESP_OK) {
        _running = false;
        return false;
    }
    if (_stereo) {
        ledc_channel_config_t chConfR = chConf;
        chConfR.gpio_num = _pin + 1;
        chConfR.channel  = chR;
        if (ledc_channel_config(&chConfR) != ESP_OK) {
            _running = false;
            return false;
        }
    }

    // リングバッファ確保 (内部RAM = ISR から安全にアクセス可能)
    delete[] _ring;
    _ringSize = _buffers * _bufferWords;
    _ring = new uint32_t[_ringSize];
    if (!_ring) {
        _ringSize = 0;
        _running = false;
        return false;
    }

    // LEDC タイマーオーバーフロー割り込みを有効化し、ISR を登録
    LEDC.int_ena.val |= LEDC_LSTIMER0_OVF_INT_ENA;
    if (ledc_isr_register(PWMAudio::isr, this, ESP_INTR_FLAG_IRAM, &_isrHandle) != ESP_OK) {
        _running = false;
        return false;
    }
    return true;
}

bool PWMAudio::end() {
    if (_running) {
        _running = false;
        // 割り込み無効化・ISR解放
        LEDC.int_ena.val &= ~LEDC_LSTIMER0_OVF_INT_ENA;
        if (_isrHandle) {
            esp_intr_free((intr_handle_t)_isrHandle);
            _isrHandle = nullptr;
        }
        // duty 0 (無音)
        LEDC.channel_group[speed_mode].channel[chL].duty.duty = 0;
        LEDC.channel_group[speed_mode].channel[chL].conf1.duty_start = 1;
        LEDC.channel_group[speed_mode].channel[chL].conf0.low_speed_update = 1;
        if (_stereo) {
            LEDC.channel_group[speed_mode].channel[chR].duty.duty = 0;
            LEDC.channel_group[speed_mode].channel[chR].conf1.duty_start = 1;
            LEDC.channel_group[speed_mode].channel[chR].conf0.low_speed_update = 1;
        }
        delete[] _ring;
        _ring = nullptr;
        _ringSize = 0;
    }
    return true;
}

// レート累算器 (Bresenham 風) で任意サンプルレートを実現。
// ISR は PWM キャリア周波数で発火し、累算器がサンプルレートを積分する。
void IRAM_ATTR PWMAudio::isr(void *arg) {
    PWMAudio *self = (PWMAudio *)arg;

    // LEDC タイマーオーバーフロー割り込みをクリア
    if (LEDC.int_st.val & LEDC_LSTIMER0_OVF_INT_ST) {
        LEDC.int_clr.val = LEDC_LSTIMER0_OVF_INT_CLR;
    }

    if (!self->_running) {
        return;
    }

    self->_acc += self->_sampleRate;
    if (self->_acc >= (uint32_t)self->_freq) {
        self->_acc -= self->_freq;

        uint32_t word;
        if (self->_head != self->_tail) {
            word = self->_ring[self->_head];
            self->_head = (self->_head + 1) % self->_ringSize;
        } else {
            word = 0x80008000; // アンダーフロー: 無音
            self->_overunderflow = true;
        }

        uint32_t l = word & 0xffff;         // CH0 = L
        uint32_t r = (word >> 16) & 0xffff; // CH1 = R
        uint32_t dutyL = (l * self->_pwmMax) >> 16;
        uint32_t dutyR = (r * self->_pwmMax) >> 16;

        LEDC.channel_group[speed_mode].channel[chL].duty.duty = dutyL << 4;
        LEDC.channel_group[speed_mode].channel[chL].conf1.duty_start = 1;
        LEDC.channel_group[speed_mode].channel[chL].conf0.low_speed_update = 1;
        if (self->_stereo) {
            LEDC.channel_group[speed_mode].channel[chR].duty.duty = dutyR << 4;
            LEDC.channel_group[speed_mode].channel[chR].conf1.duty_start = 1;
            LEDC.channel_group[speed_mode].channel[chR].conf0.low_speed_update = 1;
        }

        // バッファ1個分消費するごとにコールバック (RP2040 の DMA IRQ 相当)
        if (++self->_consumerCount >= self->_bufferWords) {
            self->_consumerCount = 0;
            if (self->_cbd) {
                self->_cbd(self->_cbdata);
            } else if (self->_cb) {
                self->_cb();
            }
        }
    }
}

size_t PWMAudio::write(int16_t val, bool sync) {
    if (!_running) {
        return 0;
    }
    // signed -32768..32767 -> unsigned 0..65535
    uint32_t sample = (uint32_t)(val + 0x8000);
    if (!_stereo) {
        // モノラル: L/R 両チャネルに同じ値を出力
        sample = (sample & 0xffff) | (sample << 16);
        size_t next = (_tail + 1) % _ringSize;
        if (next == _head) {
            if (!sync) {
                return 0;
            }
            while (next == _head) {
                yield();
            }
        }
        _ring[_tail] = sample;
        _tail = next;
        return 1;
    } else {
        // ステレオ: L を保持し、次の R と組み合わせて1ワード書く
        if (_wasHolding) {
            _holdWord = (_holdWord & 0xffff) | (sample << 16);
            size_t next = (_tail + 1) % _ringSize;
            if (next == _head) {
                if (!sync) {
                    return 0;
                }
                while (next == _head) {
                    yield();
                }
            }
            _ring[_tail] = _holdWord;
            _tail = next;
            _wasHolding = false;
            return 1;
        } else {
            _holdWord = sample;
            _wasHolding = true;
            return 1;
        }
    }
}

int PWMAudio::available() {
    return availableForWrite();
}

int PWMAudio::availableForWrite() {
    if (!_running || !_ringSize) {
        return 0;
    }
    size_t used = (_tail - _head + _ringSize) % _ringSize;
    return (_ringSize - 1 - used) * 4; // バイト単位 (RP2040 と同じ)
}

void PWMAudio::flush() {
    if (!_running) {
        return;
    }
    while (_tail != _head) {
        yield();
    }
}

void PWMAudio::onTransmit(void (*cb)(void)) {
    _cb = cb;
}

void PWMAudio::onTransmit(void (*cb)(void *), void *data) {
    _cbd = cb;
    _cbdata = data;
}

bool PWMAudio::getUnderflow() {
    if (!_running) {
        return false;
    }
    bool hold = _overunderflow;
    _overunderflow = false;
    return hold;
}