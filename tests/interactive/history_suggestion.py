#!/usr/bin/env python3
"""PTY regression for wrapped history suggestion redraw and acceptance."""
import fcntl
import os
import pty
import re
import select
import struct
import sys
import tempfile
import time
import termios


binary = os.path.abspath(sys.argv[1])


def read_until(fd, marker, timeout=4):
    output = b""
    deadline = time.monotonic() + timeout
    while marker not in output and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
        if not ready:
            break
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
    if marker not in output:
        raise AssertionError(f"timed out waiting for {marker!r}; received {output!r}")
    return output


def read_for(fd, duration=0.3):
    output = b""
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], deadline - time.monotonic())
        if not ready:
            break
        try:
            output += os.read(fd, 4096)
        except OSError:
            break
    return output


class Screen:
    """Small terminal model for the renderer's cursor and clear sequences."""

    def __init__(self, rows=8, columns=24):
        self.cells = [[" "] * columns for _ in range(rows)]
        self.row = 0
        self.column = 0
        self.saved = (0, 0)

    def feed(self, data):
        i = 0
        while i < len(data):
            if data[i:i + 2] == b"\x1b7":
                self.saved = (self.row, self.column)
                i += 2
                continue
            if data[i:i + 2] == b"\x1b8":
                self.row, self.column = self.saved
                i += 2
                continue
            if data[i:i + 2] == b"\x1b[":
                match = re.match(rb"\x1b\[([0-9;?]*)([A-Za-z])", data[i:])
                if match:
                    raw, action = match.groups()
                    if raw.startswith(b"?"):
                        i += len(match.group(0))
                        continue
                    values = [int(value) if value else 0 for value in raw.split(b";") if value]
                    amount = values[0] if values and values[0] else 1
                    if action == b"A":
                        self.row = max(0, self.row - amount)
                    elif action == b"B":
                        self.row = min(len(self.cells) - 1, self.row + amount)
                    elif action == b"C":
                        self.column = min(len(self.cells[0]) - 1, self.column + amount)
                    elif action == b"D":
                        self.column = max(0, self.column - amount)
                    elif action == b"G":
                        self.column = max(0, amount - 1)
                    elif action == b"H":
                        self.row = max(0, (values[0] if values and values[0] else 1) - 1)
                        self.column = max(0, (values[1] if len(values) > 1 and values[1] else 1) - 1)
                    elif action == b"X":
                        for col in range(self.column, min(self.column + amount, len(self.cells[0]))):
                            self.cells[self.row][col] = " "
                    elif action == b"J" and (not values or values[0] in (0, 2)):
                        if values and values[0] == 2:
                            self.cells = [[" "] * len(self.cells[0]) for _ in self.cells]
                        else:
                            for row in range(self.row, len(self.cells)):
                                start = self.column if row == self.row else 0
                                self.cells[row][start:] = [" "] * (len(self.cells[row]) - start)
                    i += len(match.group(0))
                    continue
            char = data[i]
            if char == 13:
                self.column = 0
            elif char == 10:
                self.row = min(len(self.cells) - 1, self.row + 1)
            elif char >= 32 and char != 127:
                if self.row < len(self.cells) and self.column < len(self.cells[0]):
                    self.cells[self.row][self.column] = chr(char)
                self.column += 1
            i += 1

    def text(self):
        return "".join("".join(row) for row in self.cells)


with tempfile.TemporaryDirectory(prefix="dax-history-suggestion-") as home:
    target = os.path.join(home, "accepted")
    command = f"echo dax-wrapped-history-suggestion > {target}"
    env = os.environ.copy()
    env["HOME"] = home
    env["TERM"] = "xterm"
    env.pop("NO_COLOR", None)

    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)

    try:
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 24, 0, 0))
        read_until(fd, b"> ")

        os.write(fd, (command + "\n").encode())
        read_until(fd, b"> ")

        prefix = b"echo dax-wrapped-history-"
        os.write(fd, prefix)
        suggested = read_for(fd)
        screen = Screen()
        screen.feed(suggested)
        if "echo dax-wrapped-history-suggestion" not in screen.text():
            raise AssertionError(f"wrapped suggestion was not present on screen: {screen.text()!r}")
        if b"\x1b[90m" not in suggested:
            raise AssertionError("wrapped history suffix was not rendered as a suggestion")

        # Force two redraws while the suggested suffix crosses terminal rows.
        os.write(fd, b"x")
        edited = read_for(fd)
        edited_screen = Screen()
        edited_screen.feed(edited)
        if "dax-wrapped-history-suggestion" in edited_screen.text():
            raise AssertionError("editing at the end did not remove the wrapped suggestion")
        os.write(fd, b"\x7f")
        redrawn = read_for(fd)
        screen = Screen()
        screen.feed(redrawn)
        if "echo dax-wrapped-history-suggestion" not in screen.text():
            raise AssertionError("redraw did not restore the wrapped suggestion")

        # Submitting without accepting removes the suffix from the screen.
        os.write(fd, b"\n")
        submitted = read_until(fd, b"> ")
        screen.feed(submitted)
        if "suggestion" in screen.text():
            raise AssertionError("unaccepted suggestion remained visible after submission")

        os.write(fd, prefix)
        read_for(fd)
        # Right accepts the full suggestion; executing it proves the hidden
        # wrapped suffix was inserted into Readline's actual input buffer.
        os.write(fd, b"\x1b[C\n")
        read_until(fd, b"> ")
        if not os.path.isfile(target):
            raise AssertionError("accepting a wrapped history suggestion did not run the full command")
        with open(target, encoding="utf-8") as accepted:
            if accepted.read().strip() != "dax-wrapped-history-suggestion":
                raise AssertionError("accepted wrapped suggestion executed with incomplete text")

        os.write(fd, b"exit\n")
        _, status = os.waitpid(pid, 0)
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            raise AssertionError(f"Dax exited unexpectedly: wait status {status}")
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass

print("Dax wrapped history suggestion PTY check passed")
