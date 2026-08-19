#!/bin/sh
set -e
TMP_DIR="/tmp/speedtest-install"
URL="https://install.speedtest.net/app/cli/ookla-speedtest-1.2.0-linux-aarch64.tgz"
mkdir -p "$TMP_DIR"
cd "$TMP_DIR"
wget -q "$URL" -O speedtest.tgz
tar xzf speedtest.tgz
mv speedtest /usr/bin/
chmod +x /usr/bin/speedtest
cd /
rm -rf "$TMP_DIR"
echo "[OK] Speedtest CLI installé"
