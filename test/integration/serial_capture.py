#!/usr/bin/env python3
"""Simple serial capture utility using pyserial.

Usage:
    python test/integration/serial_capture.py /dev/ttyUSB0 115200 out.log 30

Captures for duration seconds and writes to out.log
"""
import sys
import time
from pathlib import Path

try:
    import serial
except Exception as e:
    print('pyserial not installed:', e)
    sys.exit(2)

if len(sys.argv) < 5:
    print('Usage: serial_capture.py <port> <baud> <out_file> <duration_sec>')
    sys.exit(2)

port = sys.argv[1]
baud = int(sys.argv[2])
out_file = Path(sys.argv[3])
duration = int(sys.argv[4])

print(f'Opening {port} at {baud} for {duration}s, logging to {out_file}')
try:
    ser = serial.Serial(port, baud, timeout=1)
except Exception as e:
    print('Failed to open serial port:', e)
    sys.exit(3)

end = time.time() + duration
with out_file.open('wb') as f:
    while time.time() < end:
        try:
            data = ser.read(ser.in_waiting or 1)
            if data:
                f.write(data)
                f.flush()
        except Exception as e:
            print('Serial read error:', e)
            break

ser.close()
print('Capture complete')
