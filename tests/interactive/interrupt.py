#!/usr/bin/env python3
import os
import pty
import select
import signal
import sys
import tempfile
import time


binary = os.path.abspath(sys.argv[1])
workspace_temp = tempfile.TemporaryDirectory(prefix="dax-interrupt-")
workspace = workspace_temp.name


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


def run_case(multiline):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(binary, [binary])

    try:
        read_until(fd, b"> ")
        if multiline:
            stale = os.path.join(workspace, "stale-multiline")
            os.write(fd, f"if true; then touch {stale}\n".encode())
            read_until(fd, b"... ")
        else:
            stale = os.path.join(workspace, "stale-partial")
            os.write(fd, f"touch {stale}".encode())
            time.sleep(0.1)

        os.write(fd, b"\x03")
        read_until(fd, b"> ")

        fresh = os.path.join(workspace, "fresh-multiline" if multiline else "fresh-partial")
        os.write(fd, f"touch {fresh}\n".encode())
        read_until(fd, b"> ")
        if not os.path.exists(fresh):
            raise AssertionError("Dax did not accept a fresh command after Ctrl+C")
        if os.path.exists(stale):
            raise AssertionError("input from before Ctrl+C was executed")

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


run_case(False)
run_case(True)

resume_output = os.path.join(workspace, "suspend-resume.out")
input_fd, command_fd = os.pipe()
output_fd = os.open(resume_output, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
pid = os.fork()
if pid == 0:
    os.dup2(input_fd, 0)
    os.dup2(output_fd, 1)
    os.dup2(output_fd, 2)
    os.close(input_fd)
    os.close(command_fd)
    os.close(output_fd)
    os.execv(binary, [binary])

os.close(input_fd)
os.close(output_fd)
try:
    os.write(command_fd, b"suspend\necho suspended-shell-resumed\nexit\n")
finally:
    os.close(command_fd)

deadline = time.monotonic() + 4
stopped = False
while time.monotonic() < deadline:
    waited, status = os.waitpid(pid, os.WNOHANG | os.WUNTRACED)
    if waited == pid:
        if os.WIFSTOPPED(status):
            stopped = True
            break
        raise AssertionError(f"Dax exited instead of suspending: wait status {status}")
    time.sleep(0.01)
if not stopped:
    os.kill(pid, signal.SIGCONT)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    raise AssertionError("suspend did not stop the Dax process")

os.kill(pid, signal.SIGCONT)
deadline = time.monotonic() + 4
while time.monotonic() < deadline:
    waited, status = os.waitpid(pid, os.WNOHANG)
    if waited == pid:
        break
    time.sleep(0.01)
else:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    raise AssertionError("Dax did not exit after resuming from suspend")
if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
    raise AssertionError(f"Dax exited unexpectedly after resuming: {status}")
with open(resume_output, encoding="utf-8") as output_file:
    if "suspended-shell-resumed" not in output_file.read():
        raise AssertionError("Dax did not continue commands after suspend")

pid, fd = pty.fork()
if pid == 0:
    os.execv(binary, [binary, "-l"])
try:
    login_prompt = read_until(fd, b"> ")
    if b"> " not in login_prompt:
        raise AssertionError("login-shell invocation did not reach the Dax prompt")
    os.write(fd, b"exit\n")
    _, status = os.waitpid(pid, 0)
    if not os.WIFEXITED(status):
        raise AssertionError(f"login-shell Dax exited unexpectedly: {status}")
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

pid, fd = pty.fork()
if pid == 0:
    os.execv(binary, [binary, "-b"])
try:
    read_until(fd, b"> ")
    os.write(fd, b"sleep 0.2 &\n")
    notification = read_until(fd, b"Done sleep 0.2", timeout=4)
    if b"Done sleep 0.2" not in notification:
        raise AssertionError("-b did not report a completed background job at the prompt")
    os.write(fd, b"exit\n")
    _, status = os.waitpid(pid, 0)
    if not os.WIFEXITED(status):
        raise AssertionError(f"Dax exited unexpectedly after job notification: {status}")
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

workspace_temp.cleanup()
print("Dax Ctrl+C, suspend, and asynchronous job notification checks passed")
