#!/usr/bin/env python3
"""
Limnx Network Storage Server — UDP key-value store

Protocol (binary, over UDP):
  Request:  [cmd:1] [key_len:1] [val_len:2 BE] [key] [value]
  Response: [status:1] [reserved:1] [data_len:2 BE] [data]

Commands: 1=PUT, 2=GET, 3=DEL, 4=LIST
Status:   0=OK, 1=NOT_FOUND, 2=ERROR

Usage:
  python3 tools/netstor_server.py [port]
  Default port: 9999
"""

import socket
import struct
import sys

CMD_PUT  = 1
CMD_GET  = 2
CMD_DEL  = 3
CMD_LIST = 4

STATUS_OK        = 0
STATUS_NOT_FOUND = 1
STATUS_ERROR     = 2

store = {}

def make_response(status, data=b""):
    return struct.pack("!BBH", status, 0, len(data)) + data

def handle_request(data):
    if len(data) < 4:
        return make_response(STATUS_ERROR)

    cmd = data[0]
    key_len = data[1]
    val_len = struct.unpack("!H", data[2:4])[0]

    key = data[4:4 + key_len].decode("utf-8", errors="replace") if key_len > 0 else ""
    value = data[4 + key_len:4 + key_len + val_len] if val_len > 0 else b""

    if cmd == CMD_PUT:
        store[key] = value
        print(f"  PUT '{key}' ({len(value)} bytes)")
        return make_response(STATUS_OK)

    elif cmd == CMD_GET:
        if key in store:
            print(f"  GET '{key}' -> {len(store[key])} bytes")
            return make_response(STATUS_OK, store[key])
        else:
            print(f"  GET '{key}' -> NOT FOUND")
            return make_response(STATUS_NOT_FOUND)

    elif cmd == CMD_DEL:
        if key in store:
            del store[key]
            print(f"  DEL '{key}' -> OK")
            return make_response(STATUS_OK)
        else:
            print(f"  DEL '{key}' -> NOT FOUND")
            return make_response(STATUS_NOT_FOUND)

    elif cmd == CMD_LIST:
        keys = "\n".join(sorted(store.keys()))
        print(f"  LIST -> {len(store)} keys")
        return make_response(STATUS_OK, keys.encode("utf-8"))

    else:
        print(f"  Unknown cmd {cmd}")
        return make_response(STATUS_ERROR)

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"Limnx Network Storage Server listening on UDP port {port}")
    print("Press Ctrl+C to stop\n")

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            resp = handle_request(data)
            sock.sendto(resp, addr)
    except KeyboardInterrupt:
        print("\nShutting down")
    finally:
        sock.close()

if __name__ == "__main__":
    main()
