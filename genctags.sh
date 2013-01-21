##################################

#!/bin/bash
# File name: genctags.sh
# Usage: genctags.sh x86

LNX=.
ARCH=x86

if [ ! -z "$1" ];then
   ARCH="$1"
fi

if [ "$2" != "tag" ];then

   echo "Gen cscope file for $ARCH ..."
   find $LNX \
      -path "$LNX/arch/*" ! -path "$LNX/arch/$ARCH*" -prune -o \
      -path "$LNX/include/asm-*" ! -path "$LNX/include/asm-$ARCH*" -prune -o \
      -path "$LNX/tmp*" -prune -o \
      -path "$LNX/Documentation*" -prune -o \
      -path "$LNX/scripts*" -prune -o \
      -path "$LNX/drivers*" -prune -o \
      -name "*.[chxsS]" -print > cscope.files

   cscope -b -q -k
fi

echo "Gen ctags file ($ARCH)..."

EXCLUDE_PATH=`find . -path "$LNX/arch/*" ! -path "$LNX/arch/$ARCH*" -prune`
EXCLUDE_PATH2=

for path in $EXCLUDE_PATH
do
   #EXCLUDE_PATH2="$EXCLUDE_PATH2 --exclude=$path"
   EXCLUDE_PATH2="$EXCLUDE_PATH2 --exclude=`echo $path | sed 's/\.\/\(.*\)/\1/'`"
done

ctags -R $EXCLUDE_PATH2

###############################################
