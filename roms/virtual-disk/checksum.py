#!/usr/bin/env python3

# compute the checksum for the option rom
checksum = 0
contents = None
with open('option.rom.tmp', 'rb') as f:
    contents = f.read()
    for b in contents:
        checksum += b
    checksum = checksum % 256
    if checksum != 0:
        checksum = 256 - checksum

# set this as the last byte of the file
with open('option.rom', 'wb') as f:
    f.write(contents)
    f.write(bytes(8191 - len(contents)))
    f.write(checksum.to_bytes(1, 'little'))
