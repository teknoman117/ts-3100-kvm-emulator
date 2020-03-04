#include "ChipSelectUnit.hpp"

#include <cstdio>

bool ChipSelectUnit::selectsMemoryAddress(uint32_t address) const {
    if (cycleType != CycleType::Memory)
        return false;
    uint32_t mask = ~maskRegister & HardwareMask;
    return (address & mask) == (addressRegister & mask);
}

bool ChipSelectUnit::selectsIOAddress(uint16_t address) const {
    if (cycleType != CycleType::IO)
        return false;
    uint16_t mask = (~maskRegister & HardwareMask) >> 10;
    uint16_t csuAddress = (addressRegister & HardwareMask) >> 10;
    return (address & mask) == (csuAddress & mask);
}

void ChipSelectUnit::Debug(const std::string& deviceName) const {
    fprintf(stderr, " --- ChipSelectUnit %s --- \n", deviceName.c_str());
    fprintf(stderr, " addressRegister:   %08x\n", addressRegister);
    fprintf(stderr, " maskRegister:      %08x\n", maskRegister);
    fprintf(stderr, " matches memory:    %s\n", cycleType == CycleType::Memory ? "yes" : "no");
    fprintf(stderr, " enabled:           %s\n", enable ? "yes" : "no");
    if (cycleType == CycleType::Memory) {
        fprintf(stderr, " address component: %08x\n", addressRegister & HardwareMask);
        fprintf(stderr, " mask component:    %08x\n", ~maskRegister & HardwareMask);
    } else {
        fprintf(stderr, " address component: %08x\n", (addressRegister & HardwareMask) >> 10);
        fprintf(stderr, " mask component:    %08x\n", (~maskRegister & HardwareMask) >> 10);
    }
    fprintf(stderr, " --- ----------------- --- \n\n");
}
