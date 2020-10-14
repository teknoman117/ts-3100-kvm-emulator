#!/bin/bash
die() { echo "$*" 1>&2 ; exit 1; }

# Download Images
echo "> Downloading images from ftp.embeddedarm.com"
curl -s -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/binaries/3100DISK.ZIP"
curl -s -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/binaries/3100BIOS.ZIP"
curl -s -O "http://ftp.embeddedarm.com/ftp/ts-x86-sbc/old-downloads/binaries/DOS404.ZIP"

# Check integrity
echo "> Checking integrity of images"
sha256sum -c checksums.sha256sum 2>&1 >/dev/null || die "Checksum of images does NOT MATCH!"
echo "> All files look good"

# Build Flash Image
echo "> Generating roms/flash.bin"
mkdir -p roms
unzip 3100DISK.ZIP 2>&1 >/dev/null || die "Failed to extract 3100DISK.ZIP"
unzip 3100BIOS.ZIP 2>&1 >/dev/null || die "Failed to extract 3100BIOS.ZIP"
unzip DOS404.ZIP 2>&1 >/dev/null || die "Failed to extract DOS404.ZIP"
cat 3100DISK.BIN DOS404.BIN 3100BIOS.BIN > roms/flash.bin

# Cleanup
rm -rf 3100DISK.BIN DOS404.BIN 3100BIOS.BIN 3100DISK.ZIP 3100BIOS.ZIP DOS404.ZIP
echo "> Done"
