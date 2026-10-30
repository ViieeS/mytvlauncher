#!/usr/bin/env python3
"""Upload a file to a rooted webOS TV over telnet as base64 chunks, then check its md5.

Slow (about 3 MB a minute) but needs nothing on the TV beyond telnet.

usage: tvput.py <tv ip> <local file> <remote path>
"""
import base64
import hashlib
import socket
import sys
import time

from tvtelnet import run, strip_iac

CHUNK = 800


def put(ip, local, remote):
    raw = open(local, "rb").read()
    b64 = base64.b64encode(raw).decode()
    tmp = remote + ".b64"
    sock = socket.create_connection((ip, 23), timeout=30)
    sock.settimeout(0.01)
    sock.sendall(f": > {tmp}\n".encode())
    for i in range(0, len(b64), CHUNK):
        sock.sendall(f"printf '%s' '{b64[i:i + CHUNK]}' >> {tmp}\n".encode())
        time.sleep(0.015)
        try:                                   # drain the echo so the TV never blocks on output
            strip_iac(sock, sock.recv(1 << 20))
        except socket.timeout:
            pass
    sock.sendall(b"exit\n")
    time.sleep(1)
    sock.close()
    out = run(ip, f"base64 -d {tmp} > {remote} && rm -f {tmp} && md5sum {remote}")
    want = hashlib.md5(raw).hexdigest()
    ok = want in out
    print(f"{remote}: {len(raw)} bytes, {'md5 OK' if ok else 'MD5 MISMATCH: ' + out}")
    return ok


if __name__ == "__main__":
    sys.exit(0 if put(*sys.argv[1:4]) else 1)
