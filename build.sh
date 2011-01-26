#!/bin/bash

OUTPUT_DIR=./out

#make distclean
#cp config.my .config
#make oldconfig
#make -j5

make modules_install INSTALL_MOD_PATH=${OUTPUT_DIR} INSTALL_MOD_STRIP=1
cp arch/arm/boot/zImage ${OUTPUT_DIR}

find ${OUTPUT_DIR} -type f -name "*.ko" -exec mv {} ${OUTPUT_DIR} \;
