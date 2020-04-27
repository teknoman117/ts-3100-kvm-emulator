#ifndef HEXDISPLAY_HPP_
#define HEXDISPLAY_HPP_

#include "DevicePio.hpp"

// maps a serial port to a unix socket

class HexDisplay : public DevicePio
{
public:
    HexDisplay() = default;
    HexDisplay(const HexDisplay&) = delete;
    HexDisplay(HexDisplay&& port) = delete;

    virtual ~HexDisplay() = default;

    HexDisplay& operator=(const HexDisplay&) = delete;
    HexDisplay& operator=(HexDisplay&& port) = delete;

    // DevicePio implementation
    void iowrite8(uint16_t address, uint8_t value) override;
    void iowrite16(uint16_t address, uint16_t value) override;
    void iowrite32(uint16_t address, uint32_t value) override;
    void iowrite64(uint16_t address, uint64_t value) override;
};

#endif /* HEXDISPLAY_HPP_ */