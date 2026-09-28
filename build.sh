#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! command -v pio >/dev/null 2>&1; then
    echo "PlatformIO CLI (pio) wurde nicht gefunden." >&2
    exit 127
fi

case "${1:-}" in
    build)
        pio run -e esp32dev
        ;;
    all)
        pio run -e esp32dev
        if [[ -n "${PORT:-}" ]]; then
            pio run -e esp32dev -t upload --upload-port "$PORT"
            pio device monitor -e esp32dev -p "$PORT"
        else
            pio run -e esp32dev -t upload
            pio device monitor -e esp32dev
        fi
        ;;
    *)
        echo "Aufruf: $0 {build|all}" >&2
        echo "Optional: PORT=/dev/ttyUSB0 $0 all" >&2
        exit 2
        ;;
esac
