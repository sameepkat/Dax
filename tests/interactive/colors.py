#!/usr/bin/env python3
import os
import pty
import select
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1])


def run(theme="dax", no_color=False, check_suggestion=False):
    home = tempfile.TemporaryDirectory(prefix="dax-colors-")
    config = os.path.join(home.name, "xdg")
    os.makedirs(os.path.join(config, "dax"))
    with open(os.path.join(config, "dax", "init.janet"), "w") as startup:
        startup.write(f'(dax/set-colorscheme "{theme}")\n')
    env = os.environ.copy()
    env["HOME"] = home.name
    env["XDG_CONFIG_HOME"] = config
    env.pop("NO_COLOR", None)
    if no_color:
        env["NO_COLOR"] = "1"

    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)

    def read_for(seconds=0.25):
        output = b""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            try:
                output += os.read(fd, 8192)
            except OSError:
                break
        return output

    try:
        initial = read_for()
        os.write(fd, b"ls")
        valid = read_for()
        if theme == "dax" and not no_color and b"\x1b[32mls\x1b[0m" not in valid:
            raise AssertionError("known command was not highlighted green")
        if theme == "high-contrast" and not no_color and b"\x1b[1;92mls\x1b[0m" not in valid:
            raise AssertionError("high-contrast command color was not applied")
        if (theme == "no-color" or no_color) and b"\x1b[32mls\x1b[0m" in valid:
            raise AssertionError("NO_COLOR/no-color did not disable command colors")
        os.write(fd, b"\n")
        read_for()

        if check_suggestion:
            os.write(fd, b"echo dax-color-suggestion-sentinel\n")
            read_for()
            os.write(fd, b"echo dax-color-sug")
            suggestion = read_for()
            if b"\x1b[90m" not in suggestion or b"gestion-sentinel" not in suggestion:
                raise AssertionError("history suggestion was not rendered in gray")
            os.write(fd, b"x\n")
            edited = read_for()
            if b"dax-color-sugx\r\n" not in edited:
                raise AssertionError("typed text was appended after the suggestion")

        os.write(fd, b"dax-no-such-command")
        invalid = read_for()
        if theme == "dax" and not no_color and b"\x1b[31mdax-no-such-command\x1b[0m" not in invalid:
            raise AssertionError("unknown command was not highlighted red")
        if theme == "high-contrast" and not no_color and b"\x1b[1;91mdax-no-such-command\x1b[0m" not in invalid:
            raise AssertionError("high-contrast unknown-command color was not applied")
        if (theme == "no-color" or no_color) and b"\x1b[31m" in invalid:
            raise AssertionError("NO_COLOR/no-color did not disable unknown-command colors")
        os.write(fd, b"\x03")
        read_for()
        os.write(fd, b"exit\n")
        _, status = os.waitpid(pid, 0)
        if not os.WIFEXITED(status):
            raise AssertionError("Dax exited abnormally in colorscheme test")
        return initial
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
        home.cleanup()


run("dax", check_suggestion=True)
run("high-contrast")
run("no-color")
run("dax", no_color=True)
print("Dax interactive colorscheme checks passed")
