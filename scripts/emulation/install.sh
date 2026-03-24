#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

sudo apt install -y qemu-system-x86 qemu-kvm qemu-utils
chmod +x "$SCRIPT_DIR"/start_virtual_machine.sh \
    "$SCRIPT_DIR"/start_virtual_machine_with_only_iso.sh \
    "$SCRIPT_DIR"/make_hardrive.sh \
    "$SCRIPT_DIR"/start_virtual_machine_with_iso.sh
