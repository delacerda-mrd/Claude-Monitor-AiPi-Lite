#!/bin/zsh
# Run idf.py with the ESP-IDF v5.4.4 environment on the Mac.
#
#   tools/idf.sh build
#   tools/idf.sh flash monitor          # port auto-detected
#
# Why a wrapper: Homebrew's default python3 is 3.14, but IDF was installed
# against python@3.12 (export.sh looks up the venv by the *current* python3's
# version and silently fails on a mismatch). Pinning PATH fixes that.
set -e
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH"
source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" > /dev/null
cd "${0:A:h}/.."

# Auto-pick the AiPi-Lite's native USB port unless -p was given.
if [[ " $* " != *" -p "* ]]; then
  port=( /dev/cu.usbmodem*(N) )
  (( ${#port} == 1 )) && set -- -p "${port[1]}" "$@"
fi
exec idf.py "$@"
