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

# Compile C sources
C_SOURCES="kernel.c console.c kstring.c rtc.c fs.c disk.c shell.c"
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
rm -f $OBJECTS boot.o isodir/boot/mljos.bin mljos.bin

echo "Build successful! Created mljOS.iso"
