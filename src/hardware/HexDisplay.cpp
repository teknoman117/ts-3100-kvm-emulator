#include "HexDisplay.hpp"

#include <cstdio>

void HexDisplay::iowrite8(uint16_t address, uint8_t value)
{
    printf("HEX CODE (1 byte): %02x\n", value);
}

void HexDisplay::iowrite16(uint16_t address, uint16_t value)
{
    printf("HEX CODE (2 byte): %04x\n", value);
}

void HexDisplay::iowrite32(uint16_t address, uint32_t value)
{
    printf("HEX CODE (4 byte): %08x\n", value);
}

void HexDisplay::iowrite64(uint16_t address, uint64_t value)
{
    printf("HEX CODE (8 byte): %016lx\n", value);
}