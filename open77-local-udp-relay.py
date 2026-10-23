#!/usr/bin/env python3
"""Open77 local VM UDP relay (Windows side).

The game client only exempts the master run-lease check for loopback
endpoints, so this relay lets the game dial 127.0.0.1:11800 while the
masterless server (and its VM-side relay) live on 192.168.0.213:11800.

Game -> 127.0.0.1:11800 (this relay) -> 192.168.0.213:11800 (VM relay)
     -> 127.0.0.1:11800 (open77-local game socket).
"""
import os
import socket
import threading

LISTEN_IP = os.environ.get("RELAY_LISTEN_IP", "127.0.0.1")
LISTEN_PORT = int(os.environ.get("RELAY_LISTEN_PORT", "11800"))
TARGET_IP = os.environ.get("RELAY_TARGET_IP", "192.168.0.213")
TARGET_PORT = int(os.environ.get("RELAY_TARGET_PORT", "11800"))
TARGET = (TARGET_IP, TARGET_PORT)


def pump(upstream, listen_sock, client):
    while True:
        try:
            data, _ = upstream.recvfrom(65535)
        except OSError:
            return
        try:
            listen_sock.sendto(data, client)
        except OSError:
            return


def main():
    listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen.bind((LISTEN_IP, LISTEN_PORT))
    upstreams = {}
    print("udp-relay ready: %s:%d -> %s:%d" % (LISTEN_IP, LISTEN_PORT, TARGET_IP, TARGET_PORT), flush=True)
    while True:
        data, client = listen.recvfrom(65535)
        up = upstreams.get(client)
        if up is None:
            up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            up.connect(TARGET)
            upstreams[client] = up
            threading.Thread(target=pump, args=(up, listen, client), daemon=True).start()
        try:
            up.sendto(data, TARGET)
        except OSError:
            pass


if __name__ == "__main__":
    main()