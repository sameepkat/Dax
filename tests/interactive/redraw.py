#!/usr/bin/env python3
"""PTY regressions for interrupted suggestions and multiline redraws."""
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
PROMPT = b"DAX_PROMPT_READY: "
CONTINUATION_PROMPT = b"DAX_CONTINUATION_READY: "


class Screen:
    def __init__(self, rows=30, columns=80):
        self.cells = [[" "] * columns for _ in range(rows)]
        self.row = 0
        self.column = 0
        self.saved = (0, 0)
        self.wrap_pending = False

    def _linefeed(self):
        self.wrap_pending = False
        self.row += 1
        if self.row >= len(self.cells):
            self.cells.pop(0)
            self.cells.append([" "] * len(self.cells[0]))
            self.row = len(self.cells) - 1

    def feed(self, data):
        i = 0
        while i < len(data):
            if data[i:i + 2] == b"\x1b7":
                self.saved = (self.row, self.column)
                i += 2
                continue
            if data[i:i + 2] == b"\x1b8":
                self.row, self.column = self.saved
                self.wrap_pending = False
                i += 2
                continue
            if data[i:i + 2] == b"\x1b[":
                match = re.match(rb"\x1b\[([0-9;?]*)([A-Za-z])", data[i:])
                if match:
                    raw, action = match.groups()
                    i += len(match.group(0))
                    if raw.startswith(b"?"):
                        continue
                    values = [int(value) if value else 0
                              for value in raw.split(b";") if value]
                    amount = values[0] if values and values[0] else 1
                    self.wrap_pending = False
                    if action == b"A":
                        self.row = max(0, self.row - amount)
                    elif action == b"B":
                        self.row = min(len(self.cells) - 1, self.row + amount)
                    elif action == b"C":
                        self.column = min(len(self.cells[0]) - 1, self.column + amount)
                    elif action == b"D":
                        self.column = max(0, self.column - amount)
                    elif action == b"G":
                        self.column = max(0, min(len(self.cells[0]) - 1, amount - 1))
                    elif action in (b"H", b"f"):
                        row = values[0] if values and values[0] else 1
                        column = values[1] if len(values) > 1 and values[1] else 1
                        self.row = max(0, min(len(self.cells) - 1, row - 1))
                        self.column = max(0, min(len(self.cells[0]) - 1, column - 1))
                    elif action == b"X":
                        end = min(self.column + amount, len(self.cells[0]))
                        self.cells[self.row][self.column:end] = [" "] * (end - self.column)
                    elif action == b"J":
                        mode = values[0] if values else 0
                        if mode == 2:
                            self.cells = [[" "] * len(self.cells[0]) for _ in self.cells]
                        elif mode == 0:
                            self.cells[self.row][self.column:] = [" "] * (len(self.cells[0]) - self.column)
                            for row in range(self.row + 1, len(self.cells)):
                                self.cells[row] = [" "] * len(self.cells[row])
                    elif action == b"K":
                        mode = values[0] if values else 0
                        if mode == 2:
                            self.cells[self.row] = [" "] * len(self.cells[0])
                        elif mode == 0:
                            self.cells[self.row][self.column:] = [" "] * (len(self.cells[0]) - self.column)
                    continue
            char = data[i]
            i += 1
            if char == 13:
                self.column = 0
                self.wrap_pending = False
            elif char == 10:
                self._linefeed()
            elif char == 8:
                self.column = max(0, self.column - 1)
                self.wrap_pending = False
            elif char >= 32 and char != 127:
                if self.wrap_pending:
                    self._linefeed()
                    self.column = 0
                self.cells[self.row][self.column] = chr(char)
                if self.column == len(self.cells[0]) - 1:
                    self.wrap_pending = True
                else:
                    self.column += 1

    def positions(self, text):
        positions = []
        for row, cells in enumerate(self.cells):
            line = "".join(cells)
            start = 0
            while True:
                column = line.find(text, start)
                if column < 0:
                    break
                positions.append((row, column))
                start = column + 1
        return positions

    def text(self):
        return "\n".join("".join(row) for row in self.cells)


def read_until(fd, marker, timeout=4):
    output = bytearray()
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
        output.extend(chunk)
    if marker not in output:
        raise AssertionError(f"timed out waiting for {marker!r}; output={bytes(output)!r}")
    return bytes(output)


def read_for(fd, duration=0.15):
    output = bytearray()
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], deadline - time.monotonic())
        if not ready:
            break
        try:
            output.extend(os.read(fd, 4096))
        except OSError:
            break
    return bytes(output)


def start_shell(home, columns=80):
    env = os.environ.copy()
    env["HOME"] = home
    env["TERM"] = "xterm"
    env["PS1"] = "DAX_PROMPT_READY: "
    env["PS2"] = "DAX_CONTINUATION_READY: "
    env.pop("NO_COLOR", None)
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, columns, 0, 0))
    return pid, fd


def stop_shell(pid, fd):
    try:
        os.write(fd, b"exit\n")
        waited, status = os.waitpid(pid, 0)
        if waited != pid or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
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


with tempfile.TemporaryDirectory(prefix="dax-redraw-") as home:
    # A live ghost suffix must disappear on Ctrl+C, leaving only its history
    # occurrence and a clean prompt for the next command.
    pid, fd = start_shell(home)
    screen = Screen()
    try:
        screen.feed(read_until(fd, PROMPT))
        token = "dax-cancel-ghost-suffix"
        os.write(fd, f"echo {token} > {home}/history-seed\n".encode())
        screen.feed(read_until(fd, PROMPT))

        prefix = b"echo dax-cancel-ghost-"
        os.write(fd, prefix)
        suggestion = read_for(fd)
        screen.feed(suggestion)
        if b"\x1b[90m" not in suggestion or len(screen.positions(token)) != 2:
            raise AssertionError("history ghost suggestion was not displayed before Ctrl+C")
        os.write(fd, b"\x03")
        interrupted = read_until(fd, PROMPT)
        screen.feed(interrupted)
        if len(screen.positions(token)) != 1:
            raise AssertionError("Ctrl+C left the ghost suggestion visible on the terminal")

        fresh = os.path.join(home, "fresh-command")
        os.write(fd, f"touch {fresh}\n".encode())
        fresh_output = read_until(fd, PROMPT)
        screen.feed(fresh_output)
        if not os.path.exists(fresh):
            raise AssertionError(f"fresh command did not run; interrupt={interrupted!r}; output={fresh_output!r}; screen={screen.text()!r}")
    finally:
        stop_shell(pid, fd)

    # Redrawing a multiline bracketed paste must leave prior command output in
    # the same terminal cells, including after cancelling the pasted buffer.
    pid, fd = start_shell(home, columns=32)
    screen = Screen(columns=32)
    try:
        screen.feed(read_until(fd, PROMPT))
        os.write(fd, b"echo DAX_PRIOR_OUTPUT_MARKER\n")
        screen.feed(read_until(fd, PROMPT))
        prior_rows = {row: list(screen.cells[row])
                      for row, cells in enumerate(screen.cells)
                      if "DAX_PRIOR_OUTPUT_MARKER" in "".join(cells)}
        if not prior_rows:
            raise AssertionError("could not locate the prior output marker in the PTY screen")

        cancelled = os.path.join(home, "cancelled-paste")
        os.write(fd, f"touch {cancelled} \\\n".encode())
        screen.feed(read_until(fd, CONTINUATION_PROMPT))
        pasted = (
            b"DAX_PASTED_LONG_LINE_PART_ONE_\\\n"
            b"DAX_PASTED_LONG_LINE_PART_TWO_\\\n"
            b"DAX_PASTED_LONG_LINE_PART_THREE"
        )
        os.write(fd, b"\x1b[200~" + pasted + b"\x1b[201~")
        screen.feed(read_for(fd, 0.3))
        os.write(fd, b"x\x7f")
        screen.feed(read_for(fd, 0.15))
        os.write(fd, b"\x03")
        screen.feed(read_until(fd, PROMPT))
        if os.path.exists(cancelled):
            raise AssertionError("Ctrl+C executed the incomplete command before the multiline paste")

        for row, cells in prior_rows.items():
            if screen.cells[row] != cells:
                raise AssertionError("multiline paste redraw overwrote previous terminal output")
    finally:
        stop_shell(pid, fd)

print("Dax interrupt and multiline redraw PTY checks passed")
