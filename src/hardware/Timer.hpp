#ifndef TIMER_HPP_
#define TIMER_HPP_

#include <cinttypes>
#include <chrono>

#include "DevicePio.hpp"
#include "Prescalable.hpp"

struct ProgrammableIntervalTimer : public DevicePio, Prescalable {
    // 25 MHz source clock
    static constexpr uint64_t SourceClockPeriod = 40;

    enum class OperatingMode : uint8_t {
        InterruptOnTerminalCount = 0,
        HardwareRetriggerableOneShot = 1,
        RateGenerator = 2,
        SquareWaveGenerator = 3,
        SoftwareTriggeredStrobe = 4,
        HardwareTriggeredStrobe = 5,
        // exact same function as their base name
        RateGenerator_2 = 6,
        SquareWaveGenerator_2 = 7,
    };

    enum class AccessMode : uint8_t {
        LatchCountValue = 0,
        LowByteOnly = 1,
        HighByteOnly = 2,
        LowByteHighByte = 3
    };

    enum class NumberFormat : bool {
        Binary = false,
        BinaryCodedDecimal = true
    };

    enum class ByteSelect : uint8_t {
        LowByte = 0,
        HighByte = 1,
        StatusByte = 2
    };

    struct ChannelCommand {
        union {
            struct {
                NumberFormat numberFormat : 1;
                OperatingMode operatingMode : 3;
                AccessMode accessMode : 2;
                uint8_t channel : 2;
            } standard;
            struct {
                uint8_t reserved : 1;
                bool readChannel0 : 1;
                bool readChannel1 : 1;
                bool readChannel2 : 1;
                bool latchStatus : 1;
                bool latchCount : 1;
                uint8_t channel : 2;
            } readback;
            uint8_t value;
        };
    };

    struct ReadBackCommandResult {
        union {
            struct {
                NumberFormat numberFormat : 1;
                OperatingMode operatingMode : 3;
                AccessMode accessMode : 2;
                bool pendingLoad : 1;
                bool outputState : 1;
            };
            uint8_t value;
        };
    };

    uint16_t timerPrescaler;
    struct {
        // timer state
        std::chrono::high_resolution_clock::time_point lastRecord;
        uint16_t value;
        uint16_t latch;
        uint16_t reload;
        bool     pendingLoad;
        bool     waitingForLoad;
        bool     outputState;
        AccessMode accessMode;
        OperatingMode operatingMode;
        NumberFormat numberFormat;

        // access state
        ByteSelect writeByte;
        ByteSelect accessByte;
        bool       latched;
    } state[3];

    ProgrammableIntervalTimer();
    virtual ~ProgrammableIntervalTimer();

    // Prescalable methods
    void setPrescaler(uint16_t prescaler) override;

    // DevicePio methods (only implements an 8 bit interface)
    void iowrite8(uint16_t address, uint8_t data) override;
    uint8_t ioread8(uint16_t address) override;

//private:
    void writeCommand(ChannelCommand command);
    void writeRegister(uint8_t timer, uint8_t value);
    uint8_t readRegister(uint8_t timer);

private:
    void resolveTimers();
};

#endif /* TIMER_HPP_ */