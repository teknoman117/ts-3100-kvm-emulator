#ifndef SERIAL_HPP_
#define SERIAL_HPP_

#include "DevicePio.hpp"
#include "../EventLoop.hpp"

#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// maps a serial port to a unix socket

class Serial16450 : public DevicePio
{
    enum class Register : uint16_t {
        Data_DivisorLowByte = 0,
        InterruptControl_DivisorHighByte = 1,
        InterruptStatus_FifoControl = 2,
        LineControl = 3,
        ModemControl = 4,
        LineStatus = 5,
        ModemStatus = 6,
        Scratchpad = 7
    };

    EventLoop mEventLoop;
    std::string mSocketName;
    std::mutex mMutex;
    uint32_t mGSI;
    int mEventFlags;

    struct __descriptors {
        std::set<int> clients;
        int server;
        int vm;
        int irq;
        int refresh;
        int readTimer;
        int writeTimer;
        __descriptors() : clients{}, server(-1), vm(-1), irq(-1), refresh(-1),
                readTimer(-1), writeTimer(-1) {}
    } fds;

    struct {
        // direct registers
        uint8_t receive;
        uint16_t divisor;
        uint8_t interruptControl;
        uint8_t lineControl;
        uint8_t modemControl;
        uint8_t scratchpad;
        // used to construct other registers
        bool readable;
        bool writable;
        bool readInterruptFlag;
        bool writeInterruptFlag;
        bool readInterruptEnabled;
        bool writeInterruptEnabled;
    } registers;

    void handleClientEvent(int clientFd, uint32_t events);
    void reloadEventLoop();
    void triggerInterrupt();

public:
    Serial16450(const EventLoop& eventLoop);
    Serial16450(const Serial16450&) = delete;
    Serial16450(Serial16450&& port) = delete;

    virtual ~Serial16450();

    Serial16450& operator=(const Serial16450&) = delete;
    Serial16450& operator=(Serial16450&& port) = delete;

    bool start(const std::string& socketName, int vmFd, uint32_t gsi);
    void stop();

    // DevicePio implementation
    void iowrite8(uint16_t address, uint8_t value) override;
    uint8_t ioread8(uint16_t address) override;
};

#endif /* SERIAL_HPP_ */