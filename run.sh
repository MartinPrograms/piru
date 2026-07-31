#!/bin/bash

./build.sh || exit 1

# Run using QEMU
if [ ! -d "logs" ]; then
    mkdir logs
fi

if [ ! -f "HDD.img" ]; then
    echo "HDD.img not found, creating a new one..."
    qemu-img create -f qcow2 HDD.img 1G
fi

# Check if theres a --debug flag
DEBUG_FLAGS=""
if [ "$1" == "--debug" ]; then
    DEBUG_FLAGS="-s -S"
fi

qemu-system-x86_64 $DEBUG_FLAGS \
  -no-shutdown \
  -no-reboot \
  -serial file:logs/qemu.log \
  -cpu Haswell,+avx,+xsave,+invtsc,vendor=QEMU_VirtSys \
  -smp 8 \
  -d int,cpu_reset \
  -m 128M \
  -hda HDD.img \
  -cdrom ./bin/piru.iso \
  -bios /usr/share/ovmf/OVMF.fd \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device e1000,netdev=net0 \
  -monitor stdio \
  -boot order=d || exit 1