#!/usr/bin/env python3
"""Tail the AiPi-Lite's USB console to stdout without resetting the chip.

    tools/serial_log.py [PORT] [SECONDS]

Opening the port with DTR/RTS deasserted avoids the reset an `idf.py monitor`
open performs, so a running device keeps running. Needs pyserial (the ESP-IDF
venv has it).
"""
import glob
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].startswith("/dev") else None
secs = float(sys.argv[-1]) if len(sys.argv) > 1 and not sys.argv[-1].startswith("/dev") else 0
if not port:
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no /dev/cu.usbmodem* port")
    port = ports[0]

end = time.time() + secs if secs else None
while end is None or time.time() < end:
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.5
        s.dtr = False
        s.rts = False
        s.open()
        while end is None or time.time() < end:
            line = s.readline()
            if line:
                sys.stdout.write(time.strftime("%H:%M:%S ") +
                                 line.decode("utf-8", "replace").rstrip() + "\n")
                sys.stdout.flush()
    except (serial.SerialException, OSError):
        time.sleep(0.5)             # device rebooting / re-enumerating
