#ifndef I386EXCLOCKPRESCALER_HPP_
#define I386EXCLOCKPRESCALER_HPP_

#include <memory>
#include <vector>

#include "DevicePio.hpp"
#include "Prescalable.hpp"

class i386EXClockPrescaler : public DevicePio {
    std::vector<std::shared_ptr<Prescalable>> devices;
    union {
        uint16_t prescaler;
        uint8_t prescalerByte[2];
        struct {
            uint8_t prescalerLow;
            uint8_t prescalerHigh;
        };
    };

public:
    i386EXClockPrescaler(const std::vector<std::shared_ptr<Prescalable>>& devices);
    i386EXClockPrescaler(std::initializer_list<std::shared_ptr<Prescalable>> devices);
    virtual ~i386EXClockPrescaler() = default;

    // DevicePio methods (8 and 16 bit interface)
    void iowrite8(uint16_t address, uint8_t data) override;
    void iowrite16(uint16_t address, uint16_t data) override;
    uint8_t ioread8(uint16_t address) override;
    uint16_t ioread16(uint16_t address) override;
};

#endif