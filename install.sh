#!/bin/bash

KN="vmlinuz"
KV=`make kernelversion`"+"
NPROC=`cat /proc/cpuinfo | grep processor | wc -l`

TOOLS="perf pinthreads"
LOGFILE="$0.log"
ERRFILE="$0.err"

run () {
   CMD=$1

   if [ -z "$CMD" ]
   then
      return 0
   else
      echo "$CMD ..."
      ($CMD >> $LOGFILE) 2>&1 | tee -a $ERRFILE

      if [ $? -ne 0 ]
      then
         RET=$?;
         echo "[CMD FAILED] $CMD";
         exit $RET;
      fi
   fi
}

compile_tools () {
   for t in $TOOLS
   do
      if [ -d "tools/$t" ]
      then
         echo "Found tool $t. Compiling...";
         run "make -j${NPROC} -C tools/$t KV=${KV}"
      fi
   done
}

compile_kernel () {
   if [ "$1" = "full" ]
   then
      run "make -j${NPROC}";
      run "sudo make modules_install";

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
   fi

   run "sudo cp arch/x86/boot/bzImage /boot/${KN}-${KV}";
   run "sudo cp System.map /boot/System.map-${KV}";
   run "sudo cp .config /boot/config-${KV}";
}

check () {
   echo "Deploying kernel $KV -- Don't forget to update grub if it is the first time !"

   if [ ! -f ".config" ]
   then
      echo "No config found. Please generate/copy one before launching this script"
      exit;
   fi

   rm $LOGFILE;
   rm $ERRFILE;
}

check;
compile_kernel $1;
compile_tools;

## Make sure that everything is properly saved (useful when the kernel is "crashed")
sync;
