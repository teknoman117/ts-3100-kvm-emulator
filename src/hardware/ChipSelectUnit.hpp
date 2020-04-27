#ifndef CHIPSELECTUNIT_HPP_
#define CHIPSELECTUNIT_HPP_

#include <cinttypes>
#include <string>

#include "DevicePio.hpp"

struct ChipSelectUnit : public DevicePio {
    // bits that the 386EX Chip Select Units can actually operate on
    static constexpr uint32_t HardwareMask = 0x03FFF800;

    enum class Register : uint16_t {
        AddressLowWord = 0,
        AddressHighWord = 2,
        AddressMaskLowWord = 4,
        AddressMaskHighWord = 6
    };

    enum class CycleType : bool {
        Memory = true,
        IO = false
    };

    // registers
    union {
        struct {
            union {
                uint16_t addressLowRegister;
                struct {
                    uint16_t waitStates : 5;
                    uint16_t reserved1 : 2;
                    bool requireReady : 1;
                    CycleType cycleType : 1;
                    bool forceWordCycle : 1;
                    bool requireSMM : 1;
                    uint16_t addressLow : 5;
                };
            };
            union {
                uint16_t addressHighRegister;
                uint16_t addressHigh : 10;
            };
        };
        uint32_t addressRegister;
    };
    union {
        struct {
            union {
                uint16_t addressMaskLowRegister;
                struct {
                    bool enable : 1;
                    uint16_t reserved2 : 9;
                    bool requireSMMMask : 1;
                    uint16_t address : 5;
                };
            };
            union {
                uint16_t addressMaskHighRegister;
                uint16_t addressMaskHigh : 10;
            };
        };
        uint32_t maskRegister;
    };

    bool selectsMemoryAddress(uint32_t address /*in future, include processor state */) const;
    bool selectsIOAddress(uint16_t address /*in future, include processor state */) const;
    void Debug(const std::string& deviceName) const;

    constexpr ChipSelectUnit() : addressRegister(0), maskRegister(0) {}

    constexpr ChipSelectUnit(uint16_t addressHigh, uint16_t addressLow, uint16_t maskHigh,
            uint16_t maskLow)
        : addressLowRegister(addressLow), addressHighRegister(addressHigh),
          addressMaskLowRegister(maskLow), addressMaskHighRegister(maskHigh) {}

    virtual ~ChipSelectUnit() = default;

    // DevicePio implementation
    void iowrite16(uint16_t address, uint16_t value) override;
    uint16_t ioread16(uint16_t address) override;
};

#endif /* CHIPSELECTUNIT_HPP_ */