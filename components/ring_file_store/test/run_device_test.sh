#!/bin/sh
#
# FileVersion: [5] 19-11-2026 - 14:54
#
# ringFileStore - build, flash and run the ESP32-S3 example, then write the
# captured device run to testLog.md in the repository root.
# Copyright (C) 2026 Willem Aandewiel (mrWheel)
# SPDX-License-Identifier: GPL-3.0-or-later
#

#-- Usage: ./run_device_test.sh [--no-flash] [port]
#--   --no-flash  do not build or flash, only reset and capture the run
#--   port        serial device, default /dev/tty.usbserial-1430
#--
#-- testLog.md is ALWAYS written, even when the build, the flash or the
#-- capture fails: the FAIL reason and the error output then become part of
#-- the log, so a failed run is still documented.

cd "$(dirname "$0")"

PORT=/dev/tty.usbserial-1430
DO_FLASH=1

for arg in "$@"
do
  if [ "$arg" = "--no-flash" ]
  then
    DO_FLASH=0
  else
    PORT=$arg
  fi
done

#-- Locate ESP-IDF: prefer the installer activation script. It only activates
#-- when sourced from an interactive shell (it calls exit otherwise), so run
#-- it with -e to get its KEY=VALUE environment lines and export them here.
#-- The -e PATH misses the system directories and $IDF_PATH/tools (idf.py),
#-- so append SYSTEM_PATH and the IDF tools directory after importing.
ACTIVATE_IDF="$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
IDF_ENV=""
if [ -f "$ACTIVATE_IDF" ]
then
  IDF_ENV=$(sh "$ACTIVATE_IDF" -e 2>/dev/null)
fi

if [ -n "$IDF_ENV" ]
then
  while IFS= read -r envLine
  do
    case "$envLine" in
      *=*) export "${envLine%%=*}=${envLine#*=}" ;;
    esac
  done <<EOF
$IDF_ENV
EOF
  export PATH="$PATH:$SYSTEM_PATH:$IDF_PATH/tools"
elif [ -n "$IDF_PATH" ] && [ -f "$IDF_PATH/export.sh" ]
then
  . "$IDF_PATH/export.sh" >/dev/null 2>&1
elif [ -f "$HOME/.espressif/v6.0.2/esp-idf/export.sh" ]
then
  . "$HOME/.espressif/v6.0.2/esp-idf/export.sh" >/dev/null 2>&1
else
  echo "ESP-IDF not found: install v6.0.2 or set IDF_PATH" >&2
fi

ROOT=../../..
CAP_TMP=$(mktemp)
OUT_TMP=$(mktemp)
trap 'rm -f "$CAP_TMP" "$OUT_TMP"' EXIT

RESULT=""
RC=1

if [ "$DO_FLASH" = 1 ]
then
  #-- idf.py refuses to build when the active python differs from the python
  #-- the build directory was configured with: fullclean the build dir then.
  if [ -f "$ROOT/build/CMakeCache.txt" ]
  then
    CONF_PY=$(sed -n 's/^PYTHON:UNINITIALIZED=//p' "$ROOT/build/CMakeCache.txt" | head -n 1)
    ACT_PY=$(command -v python)
    CONF_REAL=$(realpath "$CONF_PY" 2>/dev/null || printf '%s' "$CONF_PY")
    ACT_REAL=$(realpath "$ACT_PY" 2>/dev/null || printf '%s' "$ACT_PY")
    if [ -n "$CONF_PY" ] && [ -n "$ACT_PY" ] && [ "$CONF_REAL" != "$ACT_REAL" ]
    then
      idf.py -C "$ROOT" fullclean > "$OUT_TMP" 2>&1
    fi
  fi

  if ! idf.py -C "$ROOT" build >> "$OUT_TMP" 2>&1
  then
    RESULT="FAIL (idf.py build failed; build output below)"
  elif ! idf.py -C "$ROOT" -p "$PORT" flash >> "$OUT_TMP" 2>&1
  then
    RESULT="FAIL (idf.py flash failed; port in use? check: lsof $PORT)"
  fi
fi

if [ -z "$RESULT" ]
then
  RC=0
  python device_capture.py "$PORT" 90 "example finished" "$CAP_TMP" \
    || RC=$?

  if [ "$RC" = 0 ]
  then
    RESULT="PASS (example finished)"
  elif [ "$RC" = 3 ]
  then
    RESULT="FAIL (cannot open port $PORT; is it free? check: lsof $PORT)"
  else
    RESULT="FAIL (example finished not seen before timeout; serial output below)"
  fi
fi

{
  echo '<!--'
  echo " FileVersion: [1] $(TZ=Europe/Amsterdam date '+%d-%m-%Y - %H:%M')"
  echo
  echo ' ringFileStore - device test log: ESP32-S3 example run captured'
  echo ' from the serial console by run_device_test.sh.'
  echo ' Copyright (C) 2026 Willem Aandewiel (mrWheel)'
  echo ' SPDX-License-Identifier: GPL-3.0-or-later'
  echo '-->'
  echo
  echo '# ringFileStore - device test log'
  echo
  echo "Run:     $(TZ=Europe/Amsterdam date '+%d-%m-%Y %H:%M') Europe/Amsterdam"
  echo "Port:    $PORT"
  echo "Flashed: $([ "$DO_FLASH" = 1 ] && echo yes || echo no)"
  echo "Result:  $RESULT"
  echo
  if [ -s "$OUT_TMP" ]
  then
    echo '## Build / flash output'
    echo
    echo '```text'
    cat "$OUT_TMP"
    echo '```'
    echo
  fi
  echo '## Serial output'
  echo
  echo '```text'
  cat "$CAP_TMP"
  echo '```'
} > "$ROOT/testLog.md"

echo "device test log written to $ROOT/testLog.md"
echo "$RESULT"
exit "$RC"
