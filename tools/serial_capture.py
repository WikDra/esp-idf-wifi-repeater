"""Non-interactive UART capture for the repeater.

Usage:  python tools/serial_capture.py [PORT] [SECONDS] [--reset]

By default the control lines (DTR/RTS) are left completely alone. On
ESP32-C5's USB-Serial/JTAG those lines are wired to EN/GPIO9, and poking
them from pySerial can strap the chip into an unexpected boot mode.
Pass --reset only when you deliberately want to restart the app.
"""
import sys
import time

import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
do_reset = "--reset" in sys.argv

port = args[0] if args else "COM3"
seconds = float(args[1]) if len(args) > 1 else 20.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
# Keep the control lines inactive so opening the port does not reset the chip.
ser.dtr = False
ser.rts = False
ser.open()

try:
    if do_reset:
        # Pulse EN (RTS) while leaving GPIO9 (DTR) released -> normal boot.
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.05)
    ser.reset_input_buffer()
    deadline = time.time() + seconds
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    sys.stdout.write(buf.decode("utf-8", errors="replace"))
finally:
    ser.close()
