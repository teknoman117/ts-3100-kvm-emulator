#!/bin/bash
die() { echo "$*" 1>&2 ; exit 1; }

# Download Images
echo "> Downloading images from ftp.embeddedarm.com"
curl -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/binaries/3100DISK.ZIP"
curl -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/binaries/3100BIOS.ZIP"
curl -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/binaries/DOS404.ZIP"
curl -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/Disks/TS-3100.ZIP"

# Check integrity
echo "> Checking integrity of images"
sha256sum -c checksums.sha256sum 2>&1 >/dev/null || die "Checksum of images does NOT MATCH!"
echo "> All files look good"

# Build Flash Image
echo "> Generating roms/flash.bin"
mkdir -p roms/drivec
unzip 3100DISK.ZIP 2>&1 >/dev/null || die "Failed to extract 3100DISK.ZIP"
unzip 3100BIOS.ZIP 2>&1 >/dev/null || die "Failed to extract 3100BIOS.ZIP"
unzip DOS404.ZIP 2>&1 >/dev/null || die "Failed to extract DOS404.ZIP"
cat 3100DISK.BIN DOS404.BIN 3100BIOS.BIN > roms/flash.bin

# Cleanup
echo "> Copying TS-3100 sample disk into drivec.img"
echo "  (sudo is being used to mount the disk image)"
sudo losetup /dev/loop0 roms/drivec.img || die "Failed to bind roms/drivec.img to /dev/loop0"
sudo partprobe /dev/loop0 || die "Failed to probe for partitions"
sudo mount /dev/loop0p1 ${PWD}/roms/drivec || die "Failed to mount /dev/loop0"
sudo unzip TS-3100.ZIP -d roms/drivec || die "Failed to extract TS-3100.ZIP contents"
sudo umount roms/drivec || die "Failed to unmount drivec"
sudo losetup -d /dev/loop0

# Cleanup
rm -rf roms/drivec TS-3100.ZIP 3100DISK.BIN DOS404.BIN 3100BIOS.BIN 3100DISK.ZIP 3100BIOS.ZIP DOS404.ZIP
echo "> Done"
