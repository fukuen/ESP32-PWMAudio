# ESP32-PWMAudio
esp32s3 pwm audio for ESP8266Audio lib

This project is a port of two upstream libraries to the ESP32-S3:

1) PWMAudio (src/PWMAudio.*)
   Original: arduino-pico libraries/PWMAudio
   https://github.com/earlephilhower/arduino-pico/tree/master/libraries/PWMAudio
   Copyright (c) 2022 Earle F. Philhower, III <earlephilhower@yahoo.com>
   License: GNU Lesser General Public License v2.1 or later (LGPL-2.1+)

2) AudioOutputPWM (src/AudioOutputPWM.*)
   Original: ESP8266Audio AudioOutputPWM
   https://github.com/earlephilhower/ESP8266Audio
   Copyright (C) 2023  Earle F. Philhower, III
   License: GNU General Public License v3 (GPL-3.0)

The wrapper class (AudioOutputPWM) derives from the ESP8266Audio
`AudioOutput` base class (GPL-3.0), so this library is distributed under
the GNU General Public License version 3.  See the full text of the GPL-3.0
license at: https://www.gnu.org/licenses/gpl-3.0.txt
