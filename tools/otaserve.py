"""Shared pieces of an over-the-air push: the image check and the HTTP server.

Both `ota_push.py` (one-shot, from the terminal) and `badge_fleet.py` (the
fleet window) hand badges an `http://` URL and wait. The two differ in how
they choose targets and report progress, not in how the bytes get served, so
that part lives here rather than in both.
"""

import hashlib
import http.server
import os
import socket
import threading


class BadImage(Exception):
    """The file handed over is not something a badge could boot."""


def inspect(path):
    """Size and md5 of a firmware image, after checking it is one.

    An ESP32 application image starts with the magic byte 0xE9. Catching the
    wrong file here beats watching a room full of badges each download it,
    reject it, and report a failure that looks like a network problem.
    """
    path = os.path.abspath(path)
    if not os.path.exists(path):
        raise BadImage(f"no such file: {path}")
    blob = open(path, "rb").read()
    if not blob:
        raise BadImage(f"{path} is empty")
    if blob[0] != 0xE9:
        raise BadImage(f"{os.path.basename(path)} does not look like an ESP32 "
                       f"image (first byte 0x{blob[0]:02X}, expected 0xE9)")
    return len(blob), hashlib.md5(blob).hexdigest()


def lan_ip(toward):
    """Address of the interface that reaches `toward`.

    Connecting a UDP socket assigns a local address without sending anything,
    so this asks the OS which interface it would route from.

    Routing toward the BROKER specifically, not toward the internet: on a
    machine with a VPN up, the internet route goes out the tunnel and returns
    an address the badges cannot reach. The badges can reach the broker by
    definition - they are talking to it - so that is the right thing to aim at.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((toward, 80))
        return s.getsockname()[0]
    finally:
        s.close()


class Server:
    """Serves one directory over HTTP for as long as an update is running."""

    def __init__(self, directory, port):
        handler = lambda *a, **k: _Quiet(*a, directory=directory, **k)

        class Threaded(http.server.ThreadingHTTPServer):
            daemon_threads = True

        self.httpd = Threaded(("0.0.0.0", port), handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def shutdown(self):
        self.httpd.shutdown()


class _Quiet(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler without the per-request stderr chatter.

    With thirty badges pulling at once the default logging buries everything
    the operator actually wants to read.
    """

    def log_message(self, fmt, *args):
        pass
