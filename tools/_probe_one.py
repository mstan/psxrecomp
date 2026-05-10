import socket, json, sys
port = int(sys.argv[1]); cnt = int(sys.argv[2])
s = socket.create_connection(('127.0.0.1', port), timeout=5)
s.settimeout(5)
s.sendall(('{"cmd":"spu_events","id":1,"count":%d}\n' % cnt).encode())
buf = b''
while True:
    try:
        c = s.recv(1 << 20)
    except socket.timeout:
        print('TIMEOUT after', len(buf), 'bytes')
        break
    if not c:
        print('EOF after', len(buf), 'bytes')
        break
    buf += c
    if b'\n' in buf:
        print('GOT NEWLINE at', buf.index(b'\n'), '/', len(buf), 'bytes total')
        break
line = buf.split(b'\n', 1)[0]
try:
    j = json.loads(line)
    print('parse OK, total=', j['total'], 'count=', j['count'], 'events_len=', len(j['events']))
except Exception as e:
    print('parse FAIL:', e, ' line len:', len(line), ' tail-300:', line[-300:])
