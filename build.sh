#!/bin/bash

# Define required dependencies
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
    if [ $? -ne 0 ]; then
        echo "Error installing dependencies."
        exit 1
    fi
else
    echo "All dependencies are satisfied."
fi

echo "Compiling Micro OS..."

echo "Generating bootsector.h..."
nasm -f bin bootsector.asm -o bootsector.bin
if [ $? -ne 0 ]; then echo "Failed to assemble bootsector.asm"; exit 1; fi
python3 -c '
import sys
with open("bootsector.bin", "rb") as f: data = f.read()
idx = data.find(b"\xB9\x00\x00\x00\x00")
if idx == -1: sys.exit(1)
offset = idx + 1
with open("bootsector.h", "w") as f:
    f.write("#ifndef BOOTSECTOR_H\n#define BOOTSECTOR_H\n\n")
    f.write(f"#define BOOTSECTOR_PATCH_OFFSET {offset}\n\n")
    f.write("static const unsigned char bootsector_data[] = {\n")
    f.write("    " + ", ".join([f"0x{b:02x}" for b in data]) + "\n")
    f.write("};\n\n#endif\n")
'
if [ $? -ne 0 ]; then echo "Failed to generate bootsector.h"; exit 1; fi

echo "Building apps..."
APP_ELFS=""
for src in apps/*.c; do
    app_name=$(basename "$src" .c)
    app_obj="apps/$app_name.o"
    app_elf="apps/$app_name.elf"
    app_bin="apps/$app_name.app"
    app_hdr="apps/${app_name}_app.h"
    app_guard=$(echo "${app_name}_app_h" | tr '[:lower:].' '[:upper:]_')

    i686-linux-gnu-gcc -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -fPIC -c "$src" -o "$app_obj"
    if [ $? -ne 0 ]; then echo "Failed to compile $src"; exit 1; fi
    i686-linux-gnu-ld -m elf_i386 -T sdk/linker.app.ld "$app_obj" -o "$app_elf"
    if [ $? -ne 0 ]; then echo "Failed to link $app_elf"; exit 1; fi
    i686-linux-gnu-objcopy -O binary "$app_elf" "$app_bin"
    if [ $? -ne 0 ]; then echo "Failed to convert $app_bin"; exit 1; fi

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
    if [ $? -ne 0 ]; then echo "Failed to generate $app_hdr"; exit 1; fi

    APP_ELFS="$APP_ELFS $app_elf"
done

# Compile C sources
C_SOURCES="kernel.c console.c kstring.c rtc.c fs.c disk.c users.c shell.c"
OBJECTS=""

for src in $C_SOURCES; do
    obj="${src%.c}.o"
    i686-linux-gnu-gcc -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then echo "Failed to compile $src"; exit 1; fi
    OBJECTS="$OBJECTS $obj"
done

# Assemble bootloader
nasm -f elf32 boot.asm -o boot.o
if [ $? -ne 0 ]; then echo "Failed to assemble boot.asm"; exit 1; fi

# Link
i686-linux-gnu-ld -m elf_i386 -T linker.ld boot.o $OBJECTS -o mljos.bin
if [ $? -ne 0 ]; then echo "Failed to link object files"; exit 1; fi

# Prepare ISO structure
mkdir -p isodir/boot/grub
cp mljos.bin isodir/boot/

echo "Creating ISO image..."
grub-mkrescue -o mljOS.iso isodir
if [ $? -ne 0 ]; then echo "Failed to create ISO"; exit 1; fi

echo "Cleaning up temporary files..."
rm -f $OBJECTS boot.o isodir/boot/mljos.bin mljos.bin $APP_ELFS

echo "Build successful! Created mljOS.iso"
