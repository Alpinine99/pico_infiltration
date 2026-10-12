"""Relay commands from the web UI to the C server.

The Pico connections are owned by the C server, so Django cannot talk to a Pico
directly. It sends one line to a small control port on the C server:

    <ACTION>\t<SECONDS>\t<PICO ID>\n        e.g.  REBOOT\t0\tsouth coridor\n
                                                  SLEEP\t120\tsouth coridor\n

and expects one line back:  "OK [text]"  or  "ERR <reason>".
(The Pico id goes last because ids can contain spaces.)
"""
import os
import stat
import socket


from django.conf import settings


def send_command(action, device, seconds=0):
    """Return (ok, message)."""
    if any(c in device for c in "\t\r\n\0"):
        return False, "invalid pico id"

    addr = getattr(settings, "PICO_CONTROL_ADDR", "/run/infil/control.sock")
    line = f"{action}\t{int(seconds)}\t{device}\n".encode()
    path = os.path.abspath(os.path.expanduser(os.path.expandvars(f"{addr}.{device}")))

    # AF_UNIX paths are limited to ~104 bytes on macOS/BSD, 108 on Linux
    if len(os.fsencode(path)) > 107:
        return False, f"socket path too long ({len(os.fsencode(path))} bytes): {path}"

    # Fail early with a clear message if it's missing or not a socket
    try:
        mode = os.stat(path).st_mode
    except FileNotFoundError:
        return False, f"socket does not exist: {path}"
    except PermissionError:
        return False, f"permission denied: {path}"
    if not stat.S_ISSOCK(mode):
        return False, f"not a unix socket: {path}"

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(3)
            s.connect(path)
            s.sendall(line)
            reply = s.recv(256).decode("utf-8").strip()
    except OSError as e:  # refused, timeout, unreachable ...
        return False, f"control server unreachable ({type(e).__name__}: {e})"

    if reply.startswith("OK"):
        return True, reply[2:].strip() or "queued"
    if reply.startswith("ERR"):
        return False, reply[3:].strip() or "rejected by control server"
    return False, reply or "no reply from control server"

