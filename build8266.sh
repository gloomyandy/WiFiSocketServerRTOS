#!/bin/bash
# setup the build system (paths may need changing)
export IDF_PATH="../ESP8266_RTOS_SDK/"
export PATH="/opt/xtensa-lx106-elf/bin":$PATH
python -m pip install --user -r ../ESP8266_RTOS_SDK//requirements.txt
# build esp8266 version, note first make may fail if not running from an interactive shell
if ! [ -f ./sdkconfig ]; then
make
fi
make
