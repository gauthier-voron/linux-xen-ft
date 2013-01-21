#!/bin/bash

KN="vmlinuz"
KV=`make kernelversion`"+"
NPROC=`cat /proc/cpuinfo | grep processor | wc -l`

run () {
   CMD=$1

   if [ -z "$CMD" ]
   then
      return 0
   else
      echo "$CMD ..."
      $CMD > /dev/null

      if [ $? -ne 0 ]
      then
         RET=$?;
         echo "[CMD FAILED] $CMD";
         exit $RET;
      fi
   fi
}

echo "Deploying kernel $KV -- Don't forget to update grub if it is the first time !"

if [ ! -f ".config" ]
then
   echo "No config found. Please generate/copy one before launching this script"
   exit;
fi

if [ "$1" = "full" ]
then
   run "make -j${NPROC}";
   run "sudo make modules_install";
   run "sudo cp arch/x86/boot/bzImage /boot/${KN}-${KV}";
   run "sudo cp System.map /boot/System.map-${KV}";
   run "sudo cp .config /boot/config-${KV}";

   sudo which mkinitramfs >/dev/null 2>&1
   if [ $? -eq 0 ]
   then
      run "sudo mkinitramfs -o /boot/initrd.img-${KV} ${KV}";
   else
      sudo which genkernel >/dev/null 2>&1
      if [ $? -eq 0 ]
      then
         run "sudo genkernel --kerneldir=`pwd` --makeopts=-j${NPROC} initramfs"
      else
         echo "Cannot build the initramfs"
      fi
   fi
else
   run "make -j${NPROC} bzImage";
   run "sudo cp System.map /boot/System.map-${KV}";
   run "sudo cp arch/x86/boot/bzImage /boot/${KN}-${KV}";
fi

## Make sure that everything is properly saved (useful when the kernel is "crashed")
sync;
