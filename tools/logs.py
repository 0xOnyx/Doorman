#!/usr/bin/env python3
"""Suit les logs reseau de l'interphone.

Usage : python3 tools/logs.py [hote] [port]   (defaut interphone.local 3333)
Se reconnecte tout seul quand la carte redemarre.
"""
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "interphone.local"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 3333

while True:
    try:
        with socket.create_connection((host, port), timeout=10) as s:
            s.settimeout(None)
            # La carte peut redemarrer sans fermer la connexion (OTA, rollback) : sans keepalive,
            # recv() attendrait indefiniment. Connexion declaree morte apres ~11 s de silence reseau.
            s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            if hasattr(socket, "TCP_KEEPIDLE"):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 5)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 2)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
            print(f"--- connecte a {host}:{port} ---", flush=True)
            while True:
                data = s.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except OSError as e:
        print(f"--- pas de connexion ({e}), nouvel essai dans 1 s ---", flush=True)
    except KeyboardInterrupt:
        sys.exit(0)
    time.sleep(1)
