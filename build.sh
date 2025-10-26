i686-linux-gnu-gcc -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -c kernel.c -o kernel.o
nasm -f elf32 boot.asm -o boot.o
i686-linux-gnu-ld -m elf_i386 -T linker.ld boot.o kernel.o -o mljos.bin
cp mljos.bin isodir/boot/
grub-mkrescue -o mljOS.iso isodir
sudo rm -r mljos.bin kernel.o boot.o isodir/boot/mljos.bin
