#!/usr/bin/env python3
import os
import sys
import termios
import time

command = sys.argv[1] if len(sys.argv) > 1 else ""

fd = os.open("/dev/ttyS1", os.O_RDWR | os.O_NOCTTY)
a = list(termios.tcgetattr(fd))
a[0] = 0
a[1] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[3] = 0
a[4] = termios.B115200
a[5] = termios.B115200
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 2
termios.tcsetattr(fd, termios.TCSANOW, a)
termios.tcflush(fd, termios.TCIOFLUSH)

if command:
    os.write(fd, command.encode("latin-1") + b"\r\n")
else:
    os.write(fd, b"\r\n")

time.sleep(2)
buf = b""
for _ in range(10):
    r = os.read(fd, 4096)
    if r:
        buf += r
    time.sleep(0.5)
os.close(fd)
print(buf.decode("latin-1"))
