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
            print(f"--- connecte a {host}:{port} ---", flush=True)
            while True:
                data = s.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except OSError as e:
        print(f"--- pas de connexion ({e}), nouvel essai dans 3 s ---", flush=True)
    except KeyboardInterrupt:
        sys.exit(0)
    time.sleep(3)
