#!/usr/bin/env python3
"""Run btop in a pty with a fixed terminal size and dump the rendered frames."""
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time

rows, cols, seconds = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
cmd = sys.argv[4:]

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execvp(cmd[0], cmd)

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

out = b""
end = time.time() + seconds
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if fd in r:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk

try:
    os.kill(pid, 15)
except ProcessLookupError:
    pass
_, status = os.waitpid(pid, 0)
sys.stdout.buffer.write(out)
sys.exit(os.WEXITSTATUS(status))
