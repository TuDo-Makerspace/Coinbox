#!/usr/bin/env bash
#
# flash_ota.sh — simple OTA uploader for ESP32
# Usage:
#   ./flash_ota.sh            # interactive
#   ./flash_ota.sh -d 192.168.4.1 -f    # firmware only
#   ./flash_ota.sh -d 192.168.4.1 -s    # spiffs only
#   ./flash_ota.sh -d 192.168.4.1 -f -s # both

set -e

# Paths to your built binaries
FW_BIN="build/coinbox.bin"
SPIFFS_BIN="build/storage.bin"

show_usage() {
  cat <<EOF
Usage: $0 [ -d <IP> ] [ -f ] [ -s ]
  -d <IP>   : ESP32 IP address (defaults to coinbox.local if not given)
  -f        : flash firmware (.bin at $FW_BIN)
  -s        : flash spiffs  (.bin at $SPIFFS_BIN)
If neither -f nor -s is given, you’ll be asked interactively.
EOF
  exit 1
}

# parse flags
IP=""
DEFAULT_IP="coinbox.local"
DO_FW=0
DO_SPIFFS=0
while getopts "d:fs" opt; do
  case "$opt" in
    d) IP="$OPTARG" ;;
    f) DO_FW=1 ;;
    s) DO_SPIFFS=1 ;;
    *) show_usage ;;
  esac
done

# prompt for IP if missing
if [[ -z "$IP" ]]; then
  read -rp "Enter ESP32 IP address [${DEFAULT_IP}]: " IP
  IP="${IP:-$DEFAULT_IP}"
fi

# validate we have curl
if ! command -v curl >/dev/null 2>&1; then
  echo "Error: curl is required for OTA uploads." >&2
  exit 1
fi

# if neither chosen, ask
if [[ $DO_FW -eq 0 && $DO_SPIFFS -eq 0 ]]; then
  echo "What do you want to flash?"
  echo "  1) Firmware ($FW_BIN)"
  echo "  2) SPIFFS   ($SPIFFS_BIN)"
  echo "  3) Both"
  read -rp "Select [1-3]: " choice
  case "$choice" in
    1) DO_FW=1 ;;
    2) DO_SPIFFS=1 ;;
    3) DO_FW=1; DO_SPIFFS=1 ;;
    *) echo "Invalid selection." >&2; exit 1 ;;
  esac
fi

# helper to upload one file
ota_upload() {
  local part="$1"; shift
  local file="$1"; shift
  if [[ ! -f "$file" ]]; then
    echo "Error: file not found: $file" >&2
    exit 1
  fi
  echo "Uploading $file → http://$IP/update?partition=$part"
  response=$(curl -X POST --data-binary @"$file" \
    "http://$IP/update?partition=$part")
  body=$(echo "$response" | sed -e 's/HTTP_STATUS\:.*//g')
  status=$(echo "$response" | tr -d '\r' | awk -F'HTTP_STATUS:' '{print $2}')
  if [[ "$status" -ge 200 && "$status" -lt 300 ]]; then
    echo "→ Success: $body"
  else
    echo "→ Error (HTTP $status): $body" >&2
    exit 1
  fi
}

# run uploads
if [[ $DO_FW -eq 1 ]]; then
  ota_upload firmware "$FW_BIN"
fi

if [[ $DO_SPIFFS -eq 1 ]]; then
  ota_upload spiffs "$SPIFFS_BIN"
fi

echo "All done."
