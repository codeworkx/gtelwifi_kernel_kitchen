#!/bin/bash

CUR_DIR=${PWD}
BIN_DIR=bin
TEMP_DIR=tmp
OUT_DIR=out
CROSS_COMPILE=${CUR_DIR}/bin/arm-eabi-4.8/bin/arm-eabi-
THREADS=`cat /proc/cpuinfo | grep processor | wc -l`

export LD_LIBRARY_PATH="$CUR_DIR/bin:$LD_LIBRARY_PATH"

mkdir -p ${TEMP_DIR}/kernel
mkdir -p ${OUT_DIR}

echo "Removing old files"
FILE=${OUT_DIR}/twrp.img
if [ -f $FILE ]; then
rm $FILE
fi

FILE=${TEMP_DIR}/dt.img
if [ -f $FILE ]; then
rm $FILE
fi

FILE=${TEMP_DIR}/ramdisk.cpio.gz
if [ -f $FILE ]; then
rm $FILE
fi

pushd kernel
echo "Building kernel using ${THREADS} threads"
make -j${THREADS} ARCH=arm gtelwifi-dt_hw07_defconfig CROSS_COMPILE=${CROSS_COMPILE} O=${CUR_DIR}/${TEMP_DIR}/kernel
make -j${THREADS} ARCH=arm CROSS_COMPILE=${CROSS_COMPILE} O=${CUR_DIR}/${TEMP_DIR}/kernel
popd

echo "Generating DTB"
${BIN_DIR}/dtbToolCM -o ${CUR_DIR}/${TEMP_DIR}/dt.img -p ${CUR_DIR}/${TEMP_DIR}/kernel/scripts/dtc/ ${CUR_DIR}/${TEMP_DIR}/kernel/arch/arm/boot/dts/

pushd ramdisk_twrp
echo "Creating ramdisk"
find . | cpio -o -H newc | gzip > ../${TEMP_DIR}/ramdisk.cpio.gz
popd

if [ -f ${CUR_DIR}/${TEMP_DIR}/kernel/arch/arm/boot/zImage ] &&  [ -f ${TEMP_DIR}/ramdisk.cpio.gz ] && [ -f ${TEMP_DIR}/dt.img ]; then
    echo "Creating twrp.img"
    mkbootimg --kernel ${CUR_DIR}/${TEMP_DIR}/kernel/arch/arm/boot/zImage --ramdisk ${TEMP_DIR}/ramdisk.cpio.gz --dt ${TEMP_DIR}/dt.img --cmdline 'console=ttyS1,115200n8' --base 0x00008000 --ramdisk_offset 0x01000000 --second_offset 0x00f00000 --tags_offset 0x00000100 --board 'sc8830' --pagesize 2048 -o ${OUT_DIR}/twrp.img
else
    echo "Can't create twrp.img because of missing input files"
fi


