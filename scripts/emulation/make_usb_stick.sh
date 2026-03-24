#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

USB_IMAGE="${1:-$ROOT_DIR/build/usb.img}"
USB_SIZE_MB="${2:-64}"

mkdir -p "$ROOT_DIR/build"
truncate -s "${USB_SIZE_MB}M" "$USB_IMAGE"
