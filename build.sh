#!/bin/bash

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
# Если build/ занят root-ом (часто после неудачных прошлых сборок), собираем в build_user/
if [ ! -w "$BUILD_DIR" ]; then
    BUILD_DIR="$ROOT_DIR/build_user"
fi
OBJ_DIR="$BUILD_DIR/obj"
APP_BUILD_DIR="$BUILD_DIR/apps"
GENERATED_INCLUDE_DIR="$BUILD_DIR/include"
ISO_DIR="$BUILD_DIR/isodir"
KERNEL_BIN="$BUILD_DIR/mljos.bin"
ISO_IMAGE="$BUILD_DIR/mljOS.iso"

INCLUDE_FLAGS="-I$ROOT_DIR/include -I$GENERATED_INCLUDE_DIR"
CFLAGS="-m64 -nostdlib -nostdinc -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone"
APP_CFLAGS="$CFLAGS -fno-asynchronous-unwind-tables -fno-unwind-tables"

DEPS="gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu nasm grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools ovmf git make gcc clang lld autoconf automake libtool mtools nasm"

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
# 1. Создаем папку limine
mkdir -p limine

# 2. Скачиваем файл только если его нет.
# В некоторых окружениях файл может быть owned by root и недоступен для перезаписи.
if [ ! -f limine/BOOTX64.EFI ]; then
    curl -L https://github.com/limine-bootloader/limine/raw/v7.x-binary/BOOTX64.EFI -o limine/BOOTX64.EFI
fi

# 3. Проверяем, что файл существует (должно быть около 232k)
ls -lh limine/BOOTX64.EFI || true

mkdir -p "$OBJ_DIR" "$APP_BUILD_DIR" "$GENERATED_INCLUDE_DIR/apps" "$GENERATED_INCLUDE_DIR/assets" "$ISO_DIR/boot/grub"
cp "$ROOT_DIR/config/grub.cfg" "$ISO_DIR/boot/grub/grub.cfg"

echo "Preparing bootloader payload headers..."
LIMINE_EFI_SRC=""
for candidate in \
    "$ROOT_DIR/limine/BOOTX64.EFI" \
    "$ROOT_DIR/limine/bin/BOOTX64.EFI" \
    "/usr/share/limine/BOOTX64.EFI" \
    "/usr/share/limine/bootx64.efi" \
    "/usr/local/share/limine/BOOTX64.EFI" \
    "/usr/local/share/limine/bootx64.efi"; do
    if [ -f "$candidate" ]; then
        LIMINE_EFI_SRC="$candidate"
        break
    fi
done

python3 -c "
import pathlib
import sys

def write_header(header_path: pathlib.Path, symbol: str, data: bytes) -> None:
    guard = header_path.name.replace('.', '_').upper()
    with header_path.open('w') as f:
        f.write(f'#ifndef {guard}\\n#define {guard}\\n\\n')
        f.write(f'static const unsigned int {symbol}_size = {len(data)};\\n')
        if data:
            f.write(f'static const unsigned char {symbol}_data[] = {{\\n')
            f.write('    ' + ', '.join(f'0x{b:02x}' for b in data) + '\\n')
            f.write('};\\n\\n')
        else:
            f.write(f'static const unsigned char {symbol}_data[] = {{0x00}};\\n\\n')
        f.write('#endif\\n')

root = pathlib.Path('$GENERATED_INCLUDE_DIR')
boot_dir = root / 'boot'
boot_dir.mkdir(parents=True, exist_ok=True)

src_raw = '$LIMINE_EFI_SRC'.strip()
src = pathlib.Path(src_raw) if src_raw else None
data = src.read_bytes() if src and src.is_file() else b''
write_header(boot_dir / 'limine_bootx64_efi.h', 'limine_bootx64_efi', data)
" 

if [ -n "$LIMINE_EFI_SRC" ]; then
    echo "Found Limine UEFI binary: $LIMINE_EFI_SRC"
else
    echo "Error: Limine BOOTX64.EFI not found."
    echo "Install Limine and ensure one of these exists:"
    echo "  - $ROOT_DIR/limine/BOOTX64.EFI"
    echo "  - $ROOT_DIR/limine/bin/BOOTX64.EFI"
    echo "  - /usr/local/share/limine/BOOTX64.EFI"
    echo "  - /usr/share/limine/BOOTX64.EFI"
    exit 1
fi

echo "Generating asset headers (wallpaper & icons)..."
python3 -c "
import pathlib
import sys

def write_header(header_path, symbol, data):
    guard = header_path.name.replace('.', '_').upper()
    with header_path.open('w') as f:
        f.write(f'#ifndef {guard}\\n#define {guard}\\n\\n')
        f.write(f'static const unsigned int {symbol}_size = {len(data)};\\n')
        if data:
            f.write(f'static const unsigned char {symbol}_data[] = {{\\n')
            for i in range(0, len(data), 20):
                chunk = data[i:i+20]
                f.write('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',\\n')
            f.write('};\\n\\n')
        else:
            f.write(f'static const unsigned char {symbol}_data[] = {{0x00}};\\n\\n')
        f.write('#endif\\n')

assets_dir = pathlib.Path('$GENERATED_INCLUDE_DIR/assets')
assets_dir.mkdir(parents=True, exist_ok=True)
root = pathlib.Path('$ROOT_DIR')

# Wallpaper
wp = root / 'wallpaper.bmp'
if wp.is_file():
    write_header(assets_dir / 'wallpaper_bmp.h', 'wallpaper_bmp', wp.read_bytes())
    print(f'  wallpaper: {wp.stat().st_size} bytes')
else:
    write_header(assets_dir / 'wallpaper_bmp.h', 'wallpaper_bmp', b'')
    print('  wallpaper: not found, empty stub')

# Icons
icons_dir = root / 'apps' / 'icons'
if icons_dir.is_dir():
    for bmp in sorted(icons_dir.glob('*.bmp')):
        sym = 'icon_' + bmp.stem + '_bmp'
        hdr = assets_dir / (sym + '.h')
        write_header(hdr, sym, bmp.read_bytes())
        print(f'  icon {bmp.stem}: {bmp.stat().st_size} bytes')
"

echo "Building apps..."
APP_ELFS=""
for src in "$ROOT_DIR"/apps/*.c; do
    app_name=$(basename "$src" .c)
    app_obj="$APP_BUILD_DIR/$app_name.o"
    app_elf="$APP_BUILD_DIR/$app_name.elf"
    app_bin="$APP_BUILD_DIR/$app_name.app"
    app_hdr="$GENERATED_INCLUDE_DIR/apps/${app_name}_app.h"
    app_guard=$(echo "${app_name}_app_h" | tr '[:lower:].' '[:upper:]_')

    x86_64-linux-gnu-gcc $APP_CFLAGS $INCLUDE_FLAGS -c "$src" -o "$app_obj"
    x86_64-linux-gnu-ld -m elf_x86_64 -T "$ROOT_DIR/config/linker.app.ld" "$app_obj" -o "$app_elf"
    x86_64-linux-gnu-objcopy -O binary "$app_elf" "$app_bin"

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

# Special case for mcrunner (it is a stub, but we build it like an app)
# It's already included in the loop above because it's in apps/*.c

echo "Compiling kernel sources..."
OBJECTS=""
for src in "$ROOT_DIR"/src/*.c; do
    obj="$OBJ_DIR/$(basename "${src%.c}").o"
    x86_64-linux-gnu-gcc $CFLAGS $INCLUDE_FLAGS -c "$src" -o "$obj"
    OBJECTS="$OBJECTS $obj"
done

echo "Assembling bootloader..."
nasm -f elf64 "$ROOT_DIR/boot/boot.asm" -o "$OBJ_DIR/boot.o"

echo "Linking kernel..."
x86_64-linux-gnu-ld -m elf_x86_64 -T "$ROOT_DIR/config/linker.ld" "$OBJ_DIR/boot.o" $OBJECTS -o "$KERNEL_BIN"

echo "Creating ISO image..."
cp "$KERNEL_BIN" "$ISO_DIR/boot/mljos.bin"
grub-mkrescue -o "$ISO_IMAGE" "$ISO_DIR"

echo "Cleaning up temporary files..."
rm -f "$ISO_DIR/boot/mljos.bin" $APP_ELFS

echo "Build successful! Created $ISO_IMAGE"
