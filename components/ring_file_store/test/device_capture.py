#
# FileVersion: [1] 19-11-2026 - 13:25
#
# ringFileStore - reset the target and capture the example run from the
# serial console until the "example finished" marker.
# Copyright (C) 2026 Willem Aandewiel (mrWheel)
# SPDX-License-Identifier: GPL-3.0-or-later
#

#-- Usage: device_capture.py <port> <timeoutSeconds> <marker> <outFile> [baud]
#-- Exits 0 when the marker was seen before the timeout, 1 on timeout and 3
#-- when the port cannot be opened. The captured raw serial bytes are
#-- written to <outFile> in every case so the caller can inspect crashed
#-- runs.

import sys
import time

import serial


def main():
    if len(sys.argv) < 5:
        sys.stderr.write("usage: device_capture.py <port> <seconds> <marker> <outFile> [baud]\n")
        return 2

    port = sys.argv[1]
    timeout = float(sys.argv[2])
    marker = sys.argv[3].encode()
    outPath = sys.argv[4]
    baud = int(sys.argv[5]) if len(sys.argv) > 5 else 115200

    data = bytearray()

    try:
        ser = serial.Serial(port, baud, timeout=0.5)
    except Exception as exc:
        sys.stderr.write("error: cannot open %s: %s\n" % (port, exc))
        return 3

    with ser:
        #-- Reset into the app like esptool's --after hard-reset: IO0 high
        #-- (DTR de-asserted), EN low (RTS asserted), then release EN.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        ser.reset_input_buffer()

        deadline = time.time() + timeout

        while time.time() < deadline:
            chunk = ser.read(512)

            if chunk:
                data.extend(chunk)

                if marker in data:
                    break

    with open(outPath, "wb") as outFile:
        outFile.write(bytes(data))

    return 0 if marker in data else 1


sys.exit(main())
