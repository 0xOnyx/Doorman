#!/usr/bin/env bash
# Envoie build/interphone-homekit.bin par OTA puis suit les logs.
# Usage : tools/ota.sh [hote]        (defaut interphone.local ; 192.168.4.1 via le SoftAP)
# Token : variable OTA_TOKEN, sinon lu dans sdkconfig (CONFIG_APP_OTA_TOKEN).
set -euo pipefail
HOST="${1:-interphone.local}"
cd "$(dirname "$0")/.."

TOKEN="${OTA_TOKEN:-$(sed -n 's/^CONFIG_APP_OTA_TOKEN="\(.*\)"$/\1/p' sdkconfig 2>/dev/null || true)}"
[ -n "$TOKEN" ] || { echo "token OTA introuvable (OTA_TOKEN ou sdkconfig)"; exit 1; }

if command -v idf.py >/dev/null 2>&1; then
    idf.py build
fi
BIN=build/interphone-homekit.bin
[ -f "$BIN" ] || { echo "$BIN introuvable : lancer idf.py build"; exit 1; }

echo "Envoi de $BIN ($(stat -c %s "$BIN") octets) vers $HOST:8080 ..."
curl -sS --fail-with-body -X POST -H "Authorization: Bearer $TOKEN" -H "Expect:" \
     --data-binary @"$BIN" "http://$HOST:8080/ota"
echo
echo "Suivi des logs dans 3 s (Ctrl-C pour quitter)"
sleep 3
exec python3 tools/logs.py "$HOST"
