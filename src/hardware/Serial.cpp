#include "Serial.hpp"

#include <array>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

#include <unistd.h>
#include <linux/kvm.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
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
    : mEventLoop(eventLoop), mSocketName{}, mMutex{}, fds{}, registers{} {}

Serial16450::~Serial16450()
{
    stop();
}

bool Serial16450::start(const std::string& socketName, int vmFd, uint32_t gsi)
{
    stop();

    // create unix socket to read content
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    mSocketName = socketName;
    strcpy(addr.sun_path, socketName.c_str());
    unlink(addr.sun_path);

    fds.server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fds.server == -1) {
        LOG_ERROR("failed to create unix socket.");
        return false;
    }

    if (bind(fds.server, (struct sockaddr*) &addr, sizeof addr) == -1) {
        LOG_ERROR("unable to bind unix socket server.");
        return false;
    }

    if (listen(fds.server, 1) == -1) {
        LOG_ERROR("unable to listen on unix server socket");
        return false;
    }

    // create interrupts
    fds.vm = vmFd;
    fds.irq = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fds.irq == -1) {
        LOG_ERROR("unable to create irq event");
        return false;
    }

    fds.refresh = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fds.refresh == -1) {
        LOG_ERROR("unable to create irq refresh event.");
        return false;
    }

    mGSI = gsi;
    struct kvm_irqfd irqfd {
        .fd = (__u32) fds.irq,
        .gsi = mGSI,
        .flags = KVM_IRQFD_FLAG_RESAMPLE,
        .resamplefd = (__u32) fds.refresh
    };
    if (ioctl(fds.vm, KVM_IRQFD, &irqfd) == -1) {
        LOG_ERROR("failed to add irq event");
        return false;
    }

    // whenever a client connects, perform the accept
    mEventLoop.addEvent(fds.server, EPOLLIN, [this] (uint32_t events) {
        if (events & EPOLLERR) {
            LOG_ERROR("error occurred with server socket.");
            return;
        }

        int fd = accept(fds.server, nullptr, nullptr);
        if (fd == -1) {
            LOG_ERROR("failed to accept client connection.");
            return;
        }

        // add client to event loop with all events disabled. interrupt enable register handles
        // what events we listen for
        std::unique_lock<std::mutex> lock(mMutex);
        mEventLoop.addEvent(fd, EPOLLIN | EPOLLOUT | EPOLLERR,
                std::bind(&Serial16450::handleClientEvent, this, fd, std::placeholders::_1));
        fds.clients.insert(fd);
    });

    // whenever the refresh event occurs, retrigger the interrupt if needed
    mEventLoop.addEvent(fds.refresh, EPOLLIN, [this] (uint32_t events) {
        if (events & EPOLLERR) {
            LOG_ERROR("error occurred with refresh event");
            return;
        }

        uint64_t data = 0;
        read(fds.refresh, &data, sizeof data);
        if ((registers.readInterruptEnabled && registers.readInterruptFlag)
                || (registers.writeInterruptEnabled && registers.writeInterruptFlag)) {
            LOG_INFO("triggering interrupt (kvm irq refresh)")
            triggerInterrupt();
        }
    });

    return true;
}

void Serial16450::stop() {
    std::unique_lock<std::mutex> lock(mMutex);

    for (int clientFd : fds.clients) {
        mEventLoop.removeEvent(clientFd);
        close(clientFd);
    }
    fds.clients.clear();

    if (fds.server != -1) {
        mEventLoop.removeEvent(fds.server);
        close(fds.server);
        fds.server = -1;
    }

    if (fds.vm != -1 && fds.irq != -1) {
        struct kvm_irqfd irqfd {
            .fd = (__u32) fds.irq,
            .gsi = mGSI,
            .flags = KVM_IRQFD_FLAG_DEASSIGN,
            .resamplefd = 0
        };
        ioctl(fds.vm, KVM_IRQFD, &irqfd);
    }
}

void Serial16450::handleClientEvent(int fd, uint32_t events)
{
    std::unique_lock<std::mutex> lock(mMutex);
    if (events & EPOLLIN) { 
        registers.readable = true;
        registers.readInterruptFlag = true;
        if (registers.readInterruptEnabled) {
            // can't fire a new interrupt unless there aren't any pending
            if (!registers.writeInterruptEnabled || !registers.writeInterruptFlag) {
                LOG_INFO("triggering interrupt (read condition)");
                triggerInterrupt();
            }
        }
    }
    if (events & EPOLLOUT) {
        registers.writable = true;
        registers.writeInterruptFlag = true;
        if (registers.writeInterruptEnabled) {
            // can't fire a new interrupt unless there aren't any pending
            if (!registers.readInterruptEnabled || !registers.readInterruptFlag) {
                LOG_INFO("triggering interrupt (write ready condition)");
                triggerInterrupt();
            }
        }
    }

    // update our listen status
    mEventLoop.modifyEvent(fd, (registers.readInterruptFlag ? 0 : EPOLLIN)
            | (registers.writeInterruptFlag ? 0 : EPOLLOUT) | EPOLLERR);
}

void Serial16450::reloadEventLoop()
{
    for (int fd : fds.clients) {
        mEventLoop.modifyEvent(fd, (registers.readInterruptFlag ? 0 : EPOLLIN)
                | (registers.writeInterruptFlag ? 0 : EPOLLOUT) | EPOLLERR);
    }
}

void Serial16450::triggerInterrupt()
{
    uint64_t data = 1;
    write(fds.irq, &data, sizeof data);
}

void Serial16450::iowrite8(uint16_t address, uint8_t data)
{
    std::unique_lock<std::mutex> lock(mMutex);

    // 16450 uart occupies 8 bytes of address space
    switch (address & 0x7) {
        case 0:
            if (!(registers.lineControl & 0x80) /* DLAB bit */) {
                for (int clientFd : fds.clients) {
                    write(clientFd, &data, 1);
                }
                registers.writable = false;
                registers.writeInterruptFlag = false;
                reloadEventLoop();
                LOG_INFO("byte was written, rearming...");
            } else {
                reinterpret_cast<uint8_t*>(&registers.divisor)[0] = data;
            }
            break;
        case 1:
            if (!(registers.lineControl & 0x80) /* DLAB bit */) {
                bool pReadInterruptEnabled = registers.readInterruptEnabled;
                bool pWriteInterruptEnabled = registers.writeInterruptEnabled;
                registers.interruptControl = data & 0x0f;
                registers.readInterruptEnabled = !!(registers.interruptControl & 0x01);
                registers.writeInterruptEnabled = !!(registers.interruptControl & 0x02);
                //LOG_INFO("interrupt control: read: " << registers.readInterruptEnabled << ", write: " << registers.writeInterruptEnabled);

                // trigger interrupt is a new interrupt condition has occurred and we weren't in
                // an interrupt cycle previously
                if ((!pReadInterruptEnabled || !registers.readInterruptFlag)
                        && (!pWriteInterruptEnabled || !registers.writeInterruptFlag)) {
                    if ((registers.readInterruptEnabled && registers.readInterruptFlag)
                            || (registers.writeInterruptEnabled && registers.writeInterruptFlag)) {
                        LOG_INFO("triggering interrupt (pending before control register write condition)")
                        triggerInterrupt();
                    }
                }
            } else {
                reinterpret_cast<uint8_t*>(&registers.divisor)[1] = data;
            }
            break;
        case 2:
            // fifo register isn't implemented in 16450
            break;
        case 3:
            registers.lineControl = data;
            break;
        case 4:
            registers.modemControl = data & 0x1F;
            break;
        case 5:
            // line status register isn't writable
            break;
        case 6:
            // modem status register isn't writable
            break;
        case 7:
            registers.scratchpad = data;
            break;
    }
}

uint8_t Serial16450::ioread8(uint16_t address)
{
    std::unique_lock<std::mutex> lock(mMutex);

    // 16450 uart occupies 8 bytes of address space
    switch (address & 0x07) {
        case 0:
            if (!(registers.lineControl & 0x80) /* DLAB bit */) {
                for (int fd : fds.clients) {
                    int n = read(fd, &registers.receive, 1);
                    if (n == 1) {
                        break;
                    }
                    // TODO: check error condition
                }
                registers.readable = false;
                registers.readInterruptFlag = false;
                reloadEventLoop();
                LOG_INFO("byte was read, rearming...");
                return registers.receive;
            }
            return reinterpret_cast<uint8_t*>(&registers.divisor)[0];
        case 1:
            if (!(registers.lineControl & 0x80) /* DLAB bit */) {
                return registers.interruptControl;
            }
            return reinterpret_cast<uint8_t*>(&registers.divisor)[1];
        case 2:
            if (registers.readInterruptEnabled && registers.readInterruptFlag) {
                return 0x04;
            }
            if (registers.writeInterruptEnabled && registers.writeInterruptFlag) {
                // reading the interrupt status register clears the write interrupt condition
                registers.writeInterruptFlag = false;
                reloadEventLoop();
                LOG_INFO("status was read during write interrupt, clearing write flag...");
                return 0x02;
            }
            // no interrupt
            return 0x01;
        case 3:
            return registers.lineControl;
        case 4:
            return registers.modemControl;
        case 5:
            // determine line status
            return (registers.writable ? 0x60 : 0x00) | (registers.readable ? 0x01 : 0x00);
        case 6:
            // modem status register: we don't implement modem controls
            return 0;
        case 7:
            return registers.scratchpad;
    }
    return 0xff;
}