#ifndef SERIAL_HPP_
#define SERIAL_HPP_

#include "DevicePio.hpp"
#include "../EventLoop.hpp"

#include <string>
#include <thread>
#include <vector>
#include <set>

// maps a serial port to a unix socket

class Serial16450 : public DevicePio
{
    EventLoop mEventLoop;
    std::string mSocketName;
    std::set<int> mClients;
    int mServerFd;

    int mVmFd;
    int mIrqFd;
    int mIrqRefreshFd;

    bool mDLAB;
    uint8_t mScratchPad;
    uint8_t mInterruptEnableRegister;
    uint16_t mBaudDivisor;

    bool isReadable();
    bool isWritable();

public:
    Serial16450(const EventLoop& eventLoop);
    Serial16450(const Serial16450&) = delete;
    Serial16450(Serial16450&& port);

    virtual ~Serial16450();

    Serial16450& operator=(const Serial16450&) = delete;
    Serial16450& operator=(Serial16450&& port);

    bool start(const std::string& socketName, int vmFd, int gsi);
    void stop();

    void iowrite8(uint16_t address, uint8_t value) override;
    uint8_t ioread8(uint16_t address) override;

    bool hasClients() {
        return mClients.size() > 0;
    }
};

#endif /* SERIAL_HPP_ */