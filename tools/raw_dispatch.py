#!/usr/bin/env python3
"""Raw TCP test for dispatch_tail."""
import socket, json, time

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', 4370))
s.sendall(b'{"cmd":"dispatch_tail","count":"8"}\n')
time.sleep(0.5)
buf = b''
while True:
    try:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        if b']}\n' in buf:
            break
    except:
        break
s.close()
print(repr(buf))
print("---")
# Find the JSON
text = buf.decode(errors='replace').strip()
print(text)
