#include "Serial.hpp"

#include <array>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#define THROW_RUNTIME_ERROR(fmt) { \
    std::ostringstream ss; \
    ss << "[FATAL] [16450 \"" << mSocketName << "\"]: "<< fmt; \
    throw std::runtime_error(ss.str()); \
};

#define LOG_ERROR(fmt) { \
    std::cerr << "[ERROR] [16450 \"" << mSocketName << "\"]: "<< fmt << std::endl; \
};

#ifndef NDEBUG
#define LOG_INFO(fmt) { \
    std::cerr << "[INFO] [16450 \"" << mSocketName << "\"]: "<< fmt << std::endl; \
};
#else
#define LOG_INFO(fmt)
#endif


Serial16450::Serial16450(const EventLoop& eventLoop)
    : mEventLoop(eventLoop), mSocketName{}, mClients{}, mServerFd{-1}, mVmFd{-1}, mIrqFd{-1},
    mIrqRefreshFd{-1}, mDLAB{false}, mScratchPad{0xff}, mInterruptEnableRegister{0}, mBaudDivisor{0} {}

Serial16450::Serial16450(Serial16450&& port)
{
    (void) operator=((Serial16450&&) port);
}

Serial16450::~Serial16450()
{
    stop();
}

Serial16450& Serial16450::operator=(Serial16450&& port)
{
    stop();

    // replace with foreign loop
    mEventLoop = std::move(port.mEventLoop);
    mClients = std::move(port.mClients);
    std::swap(mServerFd, port.mServerFd);
    std::swap(mVmFd, port.mVmFd);
    std::swap(mIrqFd, port.mIrqFd);
    std::swap(mIrqRefreshFd, port.mIrqRefreshFd);
    std::swap(mScratchPad, port.mScratchPad);
    std::swap(mBaudDivisor, port.mBaudDivisor);
    return *this;
}

bool Serial16450::start(const std::string& socketName, int vmFd, int gsi)
{
    stop();

    // create unix socket to read content
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    mSocketName = socketName;
    strcpy(addr.sun_path, socketName.c_str());
    unlink(addr.sun_path);

    mServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerFd == -1) {
        LOG_ERROR("failed to create unix socket.");
        return false;
    }

    if (bind(mServerFd, (struct sockaddr*) &addr, sizeof addr) == -1) {
        LOG_ERROR("unable to bind unix socket server.");
        return false;
    }

    if (listen(mServerFd, 1) == -1) {
        LOG_ERROR("unable to listen on unix server socket");
        return false;
    }

    bool ret = mEventLoop.addEvent(mServerFd, EPOLLIN, [this] (uint32_t events) {
        if (events & EPOLLERR) {
            // error
        }

        int clientFd = accept(mServerFd, nullptr, nullptr);
        if (clientFd == -1) {
            LOG_ERROR("failed to accept client connection.");
            return;
        }

        bool ret = mEventLoop.addEvent(clientFd, EPOLLIN | EPOLLET, [this, clientFd] (uint32_t events) {
            LOG_INFO("client " << clientFd << "data ready");
        });

        if (!ret) {
            LOG_ERROR("failed to add client connection to listen loop");
            close(clientFd);
            return;
        }
        mClients.insert(clientFd);
    });

    if (!ret) {
        LOG_ERROR("server socket failed to listen");
    }

    // create interrupts

    return true;
}

void Serial16450::stop() {
    for (int clientFd : mClients) {
        mEventLoop.removeEvent(clientFd);
        close(clientFd);
    }
    mClients.clear();

    if (mServerFd != -1) {
        mEventLoop.removeEvent(mServerFd);
        close(mServerFd);
        mServerFd = -1;
    }

    // clean up interrupts
}

bool Serial16450::isReadable() {
    // check if characters are readable
    fd_set rfd;
    FD_ZERO(&rfd);
    for (int clientFd : mClients) {
        FD_SET(clientFd, &rfd);
    }

    struct timeval timeout{0};
    int nfds = *mClients.rbegin() + 1;
    return select(nfds, &rfd, nullptr, nullptr, &timeout) > 0;
}

bool Serial16450::isWritable() {
    // check if characters are writable
    fd_set wfd;
    FD_ZERO(&wfd);
    for (int clientFd : mClients) {
        FD_SET(clientFd, &wfd);
    }

    struct timeval timeout{0};
    int nfds = *mClients.rbegin() + 1;
    return select(nfds, nullptr, &wfd, nullptr, &timeout) == mClients.size();
}

void Serial16450::iowrite8(uint16_t address, uint8_t data)
{
    // 16450 uart occupies 8 bytes of address space
    switch (address & 0x7) {
        case 0:
            if (!mDLAB) {
                for (int clientFd : mClients) {
                    write(clientFd, &data, 1);
                }
            } else {
                reinterpret_cast<uint8_t*>(&mBaudDivisor)[0] = data;
            }
            break;
        case 1:
            if (!mDLAB) {
                LOG_INFO("LOADING INTERRUPT ENABLE REGISTER = " << std::hex << (uint16_t) data);
                mInterruptEnableRegister = data & 0x0f;
            } else {
                reinterpret_cast<uint8_t*>(&mBaudDivisor)[1] = data;
            }
            break;
        case 2:
            //printf("LOADING INTERRUPT IDENTIFICATION + FIFO CONTROL REGISTER = %02x\n", *data_);
            break;
        case 3:
            mDLAB = !!(data & 0x80);
            break;
        case 4:
            //printf("LOADING MODEM CONTROL REGISTER = %02x\n", *data_);
            break;
        case 5:
            //printf("LOADING LINE STATUS REGISTER = %02x\n", *data_);
            break;
        case 6:
            //printf("LOADING MODEM STATUS REGISTER = %02x\n", *data_);
            break;
        case 7:
            mScratchPad = data;
            break;
    }
}

uint8_t Serial16450::ioread8(uint16_t address)
{
    // check if characters are readable
    fd_set fds;
    FD_ZERO(&fds);
    int nfds = mClients.size() ? (*mClients.rbegin()) + 1 : 1;
    for (int clientFd : mClients) {
        FD_SET(clientFd, &fds);
    }

    struct timeval timeout1{0}, timeout2{0};
    fd_set rfd(fds), wfd(fds);
    int numRead = select(nfds, &rfd, nullptr, nullptr, &timeout1);
    int numWrite = select(nfds, nullptr, &wfd, nullptr, &timeout2);

    // 16450 uart occupies 8 bytes of address space
    uint8_t data = 0xff;
    switch (address & 0x07) {
        case 0:
            if (!mDLAB) {
                for (int clientFd = 0; clientFd < nfds && numRead; clientFd++) {
                    if (FD_ISSET(clientFd, &rfd)) {
                        read(clientFd, &data, 1);
                        break;
                    }
                }
            } else {
                return reinterpret_cast<uint8_t*>(&mBaudDivisor)[0];
            }
            break;
        case 1:
            if (!mDLAB) {
                return mInterruptEnableRegister;
            } else {
                return reinterpret_cast<uint8_t*>(&mBaudDivisor)[1];
            }
        case 5:
            data = 0;
            if (numWrite && numWrite == mClients.size()) {
                data |= 0x60;
            }
            if (numRead > 0) {
                data |= 0x01;
            }
            break;
        case 7:
            data = mScratchPad;
            break;
        default:
            data = 0;
            break;
    }
    return data;
}