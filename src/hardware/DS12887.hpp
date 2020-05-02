#ifndef DS12887_HPP_
#define DS12887_HPP_

#include "DevicePio.hpp"

#include <array>

class DS12887 : public DevicePio
{
    enum class Register : uint8_t {
        Seconds = 0,
        SecondsAlarm = 1,
        Minutes = 2,
        MinutesAlarm = 3,
        Hours = 4,
        HoursAlarm = 5,
        Weekday = 6,
        Day = 7,
        Month = 8,
        Year = 9,
        A = 10,
        B = 11,
        C = 12,
        D = 13,
        Century = 0x32
    };

    struct {
        uint8_t secondsAlarm;
        uint8_t minutesAlarm;
        uint8_t hoursAlarm;
        struct {
            uint8_t RateSelect : 4;
            uint8_t Divider : 3;
            uint8_t zero1 : 1;
        } A;
        struct {
            bool daylightSavingsEnable : 1;
            bool twentyFourHourTime : 1;
            bool DataMode : 1;
            bool squareWaveEnable : 1;
            bool updateEndedInterruptEnable : 1;
            bool alarmInterruptEnable : 1;
            bool periodicInterruptEnable : 1;
            bool updateInhibit : 1;
        } B;
        struct {
            uint8_t zero1 : 4;
            bool updateEndedInterruptFlag : 1;
            bool alarmInterruptFlag : 1;
            bool periodicInterruptFlag : 1;
            bool interruptFlag : 1;
        } C;
        struct {
            uint8_t zero1: 7;
            bool validRamAndTime : 1;
        } D;
    } registers;

    Register selectedRegister;
    std::array<uint8_t, 114> ram;

public:
    DS12887();
    DS12887(const DS12887&) = delete;
    DS12887(DS12887&&) = delete;

    virtual ~DS12887();

    DS12887& operator=(const DS12887&) = delete;
    DS12887& operator=(DS12887&&) = delete;

    // DevicePio implementation
    void iowrite8(uint16_t address, uint8_t value) override;
    uint8_t ioread8(uint16_t address) override;
};

#endif /* DS12887_HPP_ */