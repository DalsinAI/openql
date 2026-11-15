"""Talk to openql's ser1 over TCP (-s tcp:PORT): send a line, wait for a reply.

    python3 -I tests/serial_tcp.py PORT SEND EXPECT
"""
import socket
import sys
import time

port, send, expect = int(sys.argv[1]), sys.argv[2].encode() + b"\n", sys.argv[3].encode()
for _ in range(200):
    try:
        s = socket.create_connection(("127.0.0.1", port))
        break
    except OSError:
        time.sleep(0.05)
else:
    sys.exit("serial_tcp: openql never listened")
s.sendall(send)
s.settimeout(30)
got = b""
while expect not in got:
    chunk = s.recv(256)
    if not chunk:
        break
    got += chunk
print(f"serial_tcp: {got!r}")
sys.exit(0 if expect in got else 1)
