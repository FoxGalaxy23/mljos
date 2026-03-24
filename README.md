# mljOS

`mljOS` is a small hobby operating system written in C and x86 Assembly.
It boots through GRUB2, provides a basic shell, supports a tiny in-memory filesystem, and includes simple built-in applications.

## Features

- 32-bit kernel written in freestanding C
- GRUB2-based boot process
- Basic shell with commands such as `echo`, `time`, `date`, `reboot`, and filesystem operations
- RAM filesystem and disk-backed storage support
- Multiple ATA disk detection with shell-side disk selection (`disk devices`, `disk use <n>`)
- PCI USB controller detection through the `usb` shell command
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

Create one or more virtual disks if you need them:

```bash
chmod +x scripts/emulation/*.sh
./scripts/emulation/make_hardrive.sh
./scripts/emulation/make_hardrive.sh disk1.qcow2 10G
./scripts/emulation/make_usb_stick.sh
```

Run the ISO directly:

```bash
./scripts/emulation/start_virtual_machine_with_only_iso.sh
```

Run with both disk and ISO attached:

```bash
./scripts/emulation/start_virtual_machine_with_iso.sh
```

Inside the OS, use `disk devices` to list detected ATA disks and `disk use <n>` to switch the active one.
Use `usb`, `usb ports <controller>`, `usb reset <controller> <port>`, `usb probe <controller> <port>`, `usb storage <controller> <port>`, and `usb read <controller> <port> [lba]` to inspect PCI USB controllers and UHCI root ports. If `build/usb.img` exists, the QEMU start scripts attach it as a USB mass-storage device for diagnostics.

## Notes

- `build.sh` may try to install missing packages automatically through `apt`.
- Generated files are stored in `build/` and ignored by Git.
- This project is a hobby OS and is best treated as an educational playground.
- The `usb` command currently detects PCI USB controllers, inspects UHCI root-port state, and can read a basic USB device descriptor through `usb probe`.
- `usb storage` now attempts Bulk-Only Transport SCSI discovery (`INQUIRY`, `READ CAPACITY`) for simple USB mass-storage devices on UHCI.
- `usb read` now attempts SCSI `READ(10)` for one 512-byte sector and prints a small boot-sector/MBR summary.
- Multi-disk support currently targets ATA/IDE-style drives exposed by QEMU.
