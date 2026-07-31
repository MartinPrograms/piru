#!/bin/bash

set -euo pipefail

if [ ! -d "./deps/Limine" ]; then
  echo "Limine not found, init submodules!"
  exit 1
fi

make -C deps/Limine

cmake -B build -DCMAKE_TOOLCHAIN_FILE=x86_64-elf-toolchain.cmake -D CMAKE_BUILD_TYPE=Debug .
cmake --build build --target piru

rm -rf iso
mkdir -p iso
cp -r ./limine-config/* iso/

mkdir -p iso/boot/limine
cp deps/Limine/limine-uefi-cd.bin iso/boot/limine/
cp deps/Limine/limine-bios.sys iso/boot/limine/
cp deps/Limine/limine-bios-cd.bin iso/boot/limine/

mkdir -p iso/EFI/BOOT
cp deps/Limine/BOOTX64.EFI iso/EFI/BOOT/

mkdir -p ./bin

xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso -o ./bin/piru.iso iso

sync
rm -rf iso

./deps/Limine/limine bios-install ./bin/piru.iso