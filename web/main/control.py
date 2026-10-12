"""Relay commands from the web UI to the C server.

The Pico connections are owned by the C server, so Django cannot talk to a Pico
directly. It sends one line to a small control port on the C server:

    <ACTION>\t<SECONDS>\t<PICO ID>\n        e.g.  REBOOT\t0\tsouth coridor\n
                                                  SLEEP\t120\tsouth coridor\n

and expects one line back:  "OK [text]"  or  "ERR <reason>".
(The Pico id goes last because ids can contain spaces.)
"""
import socket

from django.conf import settings


def send_command(action, device, seconds=0):
    """Return (ok, message)."""
    if any(c in device for c in "\t\r\n\0"):
        return False, "invalid pico id"

    addr = getattr(settings, "PICO_CONTROL_ADDR", ("127.0.0.1", 9091))
    line = f"{action}\t{int(seconds)}\t{device}\n".encode()

    try:
        with socket.create_connection(addr, timeout=3) as s:
            s.sendall(line)
            s.settimeout(5)
            reply = s.makefile("rb").readline(256).decode("utf-8", "replace").strip()
    except OSError as e:     # refused, timeout, unreachable ...
        return False, f"control server unreachable ({type(e).__name__})"

    if reply.startswith("OK"):
        return True, reply[2:].strip() or "queued"
    if reply.startswith("ERR"):
        return False, reply[3:].strip() or "rejected by control server"
    return False, reply or "no reply from control server"

