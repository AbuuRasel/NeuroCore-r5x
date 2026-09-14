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
#
# MaxHide hardening (SUSFS all features + max root-hide, kernel-side):
#   - SUSFS: SUS_PATH/MOUNT/AUTO_ADD_*/SUS_KSTAT/SUS_MAP/SUS_MEMFD=y
#   - SPOOF_UNAME/HIDE_SYMBOLS/SPOOF_CMDLINE/OPEN_REDIRECT/TRY_UMOUNT=y
#   - SUS_SU=y (hide su), ENABLE_LOG=n (no log leak), PROC_HIDE_KSU=y
#   - SECURITYFS=n, IKCONFIG_PROC=n, AUDIT=n
#   - KSU_VERSION=33300 (3300+30000) > all spoofed/latest managers,
#     so manager shows Working, never "Kernel update required".
set -e
cd "$(dirname "$0")"
mkdir -p out
# Enforce MaxHide on out/.config (idempotent):
if [ -f out/.config ]; then
  ./scripts/config --file out/.config \
    -d CONFIG_KSU_SUSFS_ENABLE_LOG \
    -e CONFIG_KSU_SUSFS -e CONFIG_KSU_SUSFS_SUS_PATH \
    -e CONFIG_KSU_SUSFS_SUS_MOUNT \
    -e CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT \
    -e CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT \
    -e CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT \
    -e CONFIG_KSU_SUSFS_SUS_KSTAT -e CONFIG_KSU_SUSFS_SUS_MAP \
    -e CONFIG_KSU_SUSFS_SUS_MEMFD -e CONFIG_KSU_SUSFS_SPOOF_UNAME \
    -e CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS \
    -e CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG \
    -e CONFIG_KSU_SUSFS_OPEN_REDIRECT -e CONFIG_KSU_SUSFS_TRY_UMOUNT \
    -e CONFIG_KSU_SUSFS_SUS_SU -e CONFIG_PROC_HIDE_KSU \
    -e CONFIG_ADRENO_IDLER -e CONFIG_WQ_POWER_EFFICIENT_DEFAULT \
    -d CONFIG_SECURITYFS || true
  make O=out ARCH=arm64 olddefconfig
fi
export KBUILD_BUILD_USER=android-build
export KBUILD_BUILD_HOST=android
make O=out ARCH=arm64 \
  CC=clang LD=ld.lld AR=llvm-ar AS=llvm-as NM=llvm-nm \
  OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
  CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
  KSU_GIT_VERSION_VALID=1 KSU_GIT_VERSION=3300 KSU_GIT_TAG=v3.3.0 \
  -j"$(nproc)"
# Artifacts:
#   out/arch/arm64/boot/Image.gz-dtb
#   out/arch/arm64/boot/dtbo.img
# Pack: copy both into AnyKernel3/ root and zip it (same layout as
# the released NeuroCore-v*-RMX2030-*.zip files).
