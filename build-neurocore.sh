#!/bin/bash
# NeuroCore RMX2030 build recipe (validated config).
# Environment: WSL Ubuntu 24.04, system Clang/LLVM 18.1.3,
# aarch64-linux-gnu-gcc + arm-linux-gnueabi (apt), 16 CPUs.
#
# Reference defconfig: arch/arm64/configs/vendor/RMX1911_v330_defconfig
# (HZ=300, SOUND_CONTROL=y, KSU+SUSFS options, IKCONFIG_PROC off).
# The checked-in out/.config is NOT in git; if you need to regenerate:
#   cp arch/arm64/configs/vendor/RMX1911_v330_defconfig out/.config
#   make O=out ARCH=arm64 olddefconfig
# and diff the result against a known-good build before shipping.
set -e
cd "$(dirname "$0")"
mkdir -p out
make O=out ARCH=arm64 \
  CC=clang LD=ld.lld AR=llvm-ar AS=llvm-as NM=llvm-nm \
  OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
  CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
  KSU_GIT_VERSION_VALID=1 KSU_GIT_VERSION=3214 KSU_GIT_TAG=v3.3.0 \
  -j"$(nproc)"
# Artifacts:
#   out/arch/arm64/boot/Image.gz-dtb
#   out/arch/arm64/boot/dtbo.img
# Pack: copy both into AnyKernel3/ root and zip it (same layout as
# the released NeuroCore-v*-RMX2030-*.zip files).
