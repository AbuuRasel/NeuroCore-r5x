### AnyKernel3 Ramdisk Mod Script
## NeuroCore Kernel (RMX2030, trinket) + KernelSU-Next + SUSFS v2.2.0

### AnyKernel setup
# global properties
properties() { '
kernel.string=NeuroCore Kernel by @AbuuRaseL
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=RMX1911
device.name2=RMX1912
device.name3=RMX1913
device.name4=RMX1925
device.name5=RMX2030
device.name6=RMX1919
device.name7=realme_trinket
device.name8=r5x
supported.versions=10-13
supported.patchlevels=
supported.vendorpatchlevels=
'; } # end properties


### AnyKernel install
## boot files attributes
boot_attributes() {
set_perm_recursive 0 0 755 644 $RAMDISK/*;
set_perm_recursive 0 0 750 750 $RAMDISK/init* $RAMDISK/sbin;
} # end attributes

# boot shell variables
BLOCK=/dev/block/bootdevice/by-name/boot;
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# NeuroCore details (big banner art lives in the top-level 'banner'
# file, printed first by update-binary before any checks)
ui_print " ";
ui_print "================================================";
ui_print "NeuroCore Kernel";
ui_print "BY      : Abu RaseL";
ui_print "Channel : t.me/@NeuroCoreR5x";
ui_print "KSU-N   : v3.4.0";
ui_print "SUSFS   : v2.2.0";
ui_print "Linux   : v4.14.357";
ui_print "================================================";
ui_print " ";

# boot install
ui_print "-> Dumping current boot image...";
dump_boot;

ui_print "-> Flashing NeuroCore kernel...";
write_boot;

ui_print " ";
ui_print "NeuroCore installed successfully!";
ui_print "Join : @NeuroCoreR5x";
ui_print " ";

# dtbo.img in zip root is flashed automatically by write_boot if present
## end boot install
