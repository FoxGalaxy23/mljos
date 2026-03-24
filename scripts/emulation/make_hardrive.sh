#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

mkdir -p "$ROOT_DIR/build"

DISK_NAME="${1:-disk.qcow2}"
DISK_SIZE="${2:-20G}"

qemu-img create -f qcow2 "$ROOT_DIR/build/$DISK_NAME" "$DISK_SIZE"
