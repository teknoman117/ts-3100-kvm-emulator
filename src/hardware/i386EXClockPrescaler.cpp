#include "i386EXClockPrescaler.hpp"

i386EXClockPrescaler::i386EXClockPrescaler(const std::vector<std::shared_ptr<Prescalable>>& devices)
        : devices(devices), prescaler(0) {}

i386EXClockPrescaler::i386EXClockPrescaler(std::initializer_list<std::shared_ptr<Prescalable>> devices)
        : devices(devices), prescaler(0) {}

// DevicePio methods (8 and 16 bit interface)
void i386EXClockPrescaler::iowrite8(uint16_t address, uint8_t data) { 
    prescalerByte[address & 1] = data;
    for (auto& devicePtr : devices) {
        devicePtr->setPrescaler(prescaler + 2);
    }
}

void i386EXClockPrescaler::iowrite16(uint16_t address, uint16_t data) {
    prescaler = data;
    for (auto& devicePtr : devices) {
        devicePtr->setPrescaler(prescaler + 2);
    }
}

uint8_t i386EXClockPrescaler::ioread8(uint16_t address) {
    return prescalerByte[address & 1];
}

uint16_t i386EXClockPrescaler::ioread16(uint16_t address) {
    return prescaler;
}
