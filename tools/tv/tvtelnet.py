#!/usr/bin/env python3
"""Run one shell command on a rooted webOS TV over telnet (root, no login) and print its output.

Plain sockets instead of telnetlib, which Python 3.13 removed. The markers are quoted in the
typed line, so the literal marker appears only in the output, not in the echo of the command.

usage: tvtelnet.py <tv ip> <command...>
"""
import socket
import sys
import time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


def strip_iac(sock, data):
    """Refuse every option the server asks about and drop the negotiation bytes."""
    out, i = bytearray(), 0
    while i < len(data):
        if data[i] != IAC or i + 1 >= len(data):
            out.append(data[i])
            i += 1
        elif data[i + 1] in (DO, DONT, WILL, WONT) and i + 2 < len(data):
            if data[i + 1] in (DO, WILL):
                sock.sendall(bytes([IAC, WONT if data[i + 1] == DO else DONT, data[i + 2]]))
            i += 3
        elif data[i + 1] == SB:
            end = data.find(bytes([IAC, SE]), i)
            i = len(data) if end < 0 else end + 2
        else:
            i += 2
    return bytes(out)


def run(ip, cmd, timeout=60):
    n = str(int(time.time()))
    sock = socket.create_connection((ip, 23), timeout=30)
    time.sleep(1.2)
    sock.settimeout(0.3)
    try:
        strip_iac(sock, sock.recv(65536))
    except socket.timeout:
        pass
    sock.sendall(f"echo 'BEG'{n}; {cmd}; echo 'END'{n}\n".encode())
    buf, end, deadline = b"", f"END{n}".encode(), time.time() + timeout
    while end not in buf and time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += strip_iac(sock, chunk)
    sock.sendall(b"exit\n")
    sock.close()
    body = buf.decode("utf-8", "replace")
    body = body.split(f"BEG{n}", 1)[-1].split(f"END{n}", 1)[0]
    return body.strip("\r\n")


if __name__ == "__main__":
    print(run(sys.argv[1], " ".join(sys.argv[2:]), timeout=180))
