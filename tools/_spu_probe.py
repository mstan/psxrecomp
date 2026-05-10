import socket, time, sys
port = int(sys.argv[1])
cnt = int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.settimeout(3)
s.sendall(('{"cmd":"spu_events","count":%d,"id":1}\n' % cnt).encode())
buf = b""
while b"\n" not in buf:
    try:
        chunk = s.recv(1 << 20)
    except socket.timeout:
        print("TIMEOUT, got", len(buf), "bytes")
        break
    if not chunk:
        print("EOF, got", len(buf), "bytes")
        break
    buf += chunk
print("port", port, "count", cnt, "bytes", len(buf))
if buf:
    print("preview:", buf[:200].decode(errors="replace"))
    print("tail:   ", buf[-200:].decode(errors="replace"))
