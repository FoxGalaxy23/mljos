#!/bin/bash

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OBJ_DIR="$BUILD_DIR/obj"
APP_BUILD_DIR="$BUILD_DIR/apps"
GENERATED_INCLUDE_DIR="$BUILD_DIR/include"
ISO_DIR="$BUILD_DIR/isodir"
KERNEL_BIN="$BUILD_DIR/mljos.bin"
ISO_IMAGE="$BUILD_DIR/mljOS.iso"

INCLUDE_FLAGS="-I$ROOT_DIR/include -I$GENERATED_INCLUDE_DIR"
CFLAGS="-m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector"
APP_CFLAGS="$CFLAGS -fPIC"

DEPS="gcc-i686-linux-gnu binutils-i686-linux-gnu nasm grub-pc-bin grub-common xorriso mtools"

echo "Checking for missing dependencies..."
MISSING_DEPS=""
for dep in $DEPS; do
    if ! dpkg -s "$dep" >/dev/null 2>&1; then
        MISSING_DEPS="$MISSING_DEPS $dep"
    fi
done

if [ -n "$MISSING_DEPS" ]; then
    echo "Dependencies missing: $MISSING_DEPS"
    echo "Attempting to install missing dependencies..."
    sudo apt-get update
    sudo apt-get install -y $MISSING_DEPS
else
    echo "All dependencies are satisfied."
fi

echo "Preparing build directories..."
mkdir -p "$OBJ_DIR" "$APP_BUILD_DIR" "$GENERATED_INCLUDE_DIR/apps" "$ISO_DIR/boot/grub"
cp "$ROOT_DIR/config/grub.cfg" "$ISO_DIR/boot/grub/grub.cfg"

echo "Generating bootsector.h..."
nasm -f bin "$ROOT_DIR/boot/bootsector.asm" -o "$BUILD_DIR/bootsector.bin"
python3 -c '
import pathlib
import sys

root = pathlib.Path("'"$ROOT_DIR"'")
build_dir = pathlib.Path("'"$BUILD_DIR"'")
bootsector_path = build_dir / "bootsector.bin"
header_path = build_dir / "include" / "bootsector.h"
data = bootsector_path.read_bytes()
idx = data.find(b"\xB9\x00\x00\x00\x00")
if idx == -1:
    sys.exit(1)
offset = idx + 1
with header_path.open("w") as f:
    f.write("#ifndef BOOTSECTOR_H\n#define BOOTSECTOR_H\n\n")
    f.write(f"#define BOOTSECTOR_PATCH_OFFSET {offset}\n\n")
    f.write("static const unsigned char bootsector_data[] = {\n")
    f.write("    " + ", ".join(f"0x{b:02x}" for b in data) + "\n")
    f.write("};\n\n#endif\n")
'

echo "Building apps..."
APP_ELFS=""
for src in "$ROOT_DIR"/apps/*.c; do
    app_name=$(basename "$src" .c)
    app_obj="$APP_BUILD_DIR/$app_name.o"
    app_elf="$APP_BUILD_DIR/$app_name.elf"
    app_bin="$APP_BUILD_DIR/$app_name.app"
    app_hdr="$GENERATED_INCLUDE_DIR/apps/${app_name}_app.h"
    app_guard=$(echo "${app_name}_app_h" | tr '[:lower:].' '[:upper:]_')

    i686-linux-gnu-gcc $APP_CFLAGS $INCLUDE_FLAGS -c "$src" -o "$app_obj"
    i686-linux-gnu-ld -m elf_i386 -T "$ROOT_DIR/config/linker.app.ld" "$app_obj" -o "$app_elf"
    i686-linux-gnu-objcopy -O binary "$app_elf" "$app_bin"

    python3 -c "
import pathlib
app_path = pathlib.Path('$app_bin')
header_path = pathlib.Path('$app_hdr')
guard = '$app_guard'
name = '$app_name'
data = app_path.read_bytes()
with header_path.open('w') as f:
    f.write(f'#ifndef {guard}\\n#define {guard}\\n\\n')
    f.write(f'static const unsigned int {name}_app_size = {len(data)};\\n')
    f.write(f'static const unsigned char {name}_app_data[] = {{\\n')
    f.write('    ' + ', '.join(f'0x{b:02x}' for b in data) + '\\n')
    f.write('};\\n\\n#endif\\n')
"

    APP_ELFS="$APP_ELFS $app_elf"
done

echo "Compiling kernel sources..."
OBJECTS=""
for src in "$ROOT_DIR"/src/*.c; do
    obj="$OBJ_DIR/$(basename "${src%.c}").o"
    i686-linux-gnu-gcc $CFLAGS $INCLUDE_FLAGS -c "$src" -o "$obj"
    OBJECTS="$OBJECTS $obj"
done

echo "Assembling bootloader..."
nasm -f elf32 "$ROOT_DIR/boot/boot.asm" -o "$OBJ_DIR/boot.o"

echo "Linking kernel..."
i686-linux-gnu-ld -m elf_i386 -T "$ROOT_DIR/config/linker.ld" "$OBJ_DIR/boot.o" $OBJECTS -o "$KERNEL_BIN"

echo "Creating ISO image..."
cp "$KERNEL_BIN" "$ISO_DIR/boot/mljos.bin"
grub-mkrescue -o "$ISO_IMAGE" "$ISO_DIR"

echo "Cleaning up temporary files..."
rm -f "$ISO_DIR/boot/mljos.bin" $APP_ELFS

echo "Build successful! Created $ISO_IMAGE"
