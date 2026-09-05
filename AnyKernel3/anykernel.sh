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
supported.versions=
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
ui_print "==============================================================";
ui_print "Device : Realme 5 Series";
ui_print "BY     : AbuuRaseL";
ui_print "Tele.  : t.me/@AbuuRaseL";
ui_print "KSU-N  : v3.3.0";
ui_print "SUSFS  : v2.2.0";
ui_print "Linux  : v4.14.357-openela";
ui_print "==============================================================";
ui_print " ";

# boot install
dump_boot;

write_boot;

ui_print " ";
ui_print "NeuroCore installed successfully!";
ui_print "Telegram: @AbuuRaseL";
ui_print " ";

# dtbo.img in zip root is flashed automatically by write_boot if present
## end boot install
