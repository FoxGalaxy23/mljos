# mljOS

`mljOS` is a small hobby operating system written in C and x86 Assembly.
It boots through GRUB2, provides a basic shell, supports a tiny in-memory filesystem, and includes simple built-in applications.

## Features

- 32-bit kernel written in freestanding C
- GRUB2-based boot process
- Basic shell with commands such as `echo`, `time`, `date`, `reboot`, and filesystem operations
- RAM filesystem and disk-backed storage support
- Simple user/application model with bundled apps like calculator and text editor

## Project Structure

```text
.
├── apps/                  # User-space example applications
├── boot/                  # Boot assembly sources
├── build/                 # Generated build artifacts
├── config/                # Linker scripts and GRUB config
├── include/               # Header files
├── scripts/emulation/     # QEMU helper scripts
├── src/                   # Kernel and OS source files
├── build.sh               # Main build script
└── LICENSE
```

## Requirements

On Debian/Ubuntu, the build script expects these packages:

- `gcc-i686-linux-gnu`
- `binutils-i686-linux-gnu`
- `nasm`
- `grub-pc-bin`
- `grub-common`
- `xorriso`
- `mtools`

The helper script for QEMU also uses:

- `qemu-system-x86`
- `qemu-kvm`
- `qemu-utils`

## Build

```bash
chmod +x build.sh
./build.sh
```

Build output will appear in `build/`:

- `build/mljOS.iso` - bootable ISO image
- `build/mljos.bin` - linked kernel binary
- `build/apps/` - compiled application artifacts

## Run in QEMU

Create a virtual disk if you need one:

```bash
chmod +x scripts/emulation/*.sh
./scripts/emulation/make_hardrive.sh
```

Run the ISO directly:

```bash
./scripts/emulation/start_virtual_machine_with_only_iso.sh
```

Run with both disk and ISO attached:

```bash
./scripts/emulation/start_virtual_machine_with_iso.sh
```

## Notes

- `build.sh` may try to install missing packages automatically through `apt`.
- Generated files are stored in `build/` and ignored by Git.
- This project is a hobby OS and is best treated as an educational playground.
