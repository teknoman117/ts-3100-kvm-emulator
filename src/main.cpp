#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <thread>

#include <unistd.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <Zydis/Zydis.h>

#include "AddressRange.hpp"
#include "hardware/ChipSelectUnit.hpp"
#include "hardware/Timer.hpp"
#include "hardware/i386EXClockPrescaler.hpp"
#include "hardware/Serial.hpp"

#define PAGE_SIZE 4096

typedef void (*io_handler_t)(bool is_write, uint16_t addr, void* data, size_t length, size_t count);

uint8_t rtcIndex = 0;
uint8_t rtcRegisters[128];

ChipSelectUnit chipSelectUnits[8] = {
    {}, {}, {}, {}, {}, {}, {}, { 0xFFFF, 0xFF6F, 0xFFFF, 0xFFFF },
};

ProgrammableIntervalTimer timer{};

void handlerRtc(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    uint8_t* data_ = reinterpret_cast<uint8_t*>(data);
    if (addr & 0x0001) {
        if (is_write) {
            rtcRegisters[rtcIndex] = *data_;
        } else {
            *data_ = rtcRegisters[rtcIndex];
        }
    } else {
        if (is_write) {
            rtcIndex = *data_ & 0x7f;
            // TODO: something with NMI signal;
        } else {
            fprintf(stderr, "WARN: read from the index register.\n");
        }
    }
    // really need to do something about actually measuring time here
}

// see page 5-12 (pg. 85) of 386EX manual
void handlerTimerConfiguration(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    static uint8_t register_ = 0;
    assert(length == 1);
    assert(count == 1);

    uint8_t* data_ = reinterpret_cast<uint8_t*>(data);
    if (is_write)
        register_ = *data_;
    else
        *data_ = register_;
}

void handlerPOSTCode(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);
    if (is_write) {
        fprintf(stderr, "POST CODE: %02x\n", *reinterpret_cast<uint8_t*>(data));
    }
}

void handlerKeyboard(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    //fprintf(stderr, "PC SPEAKER ACCESSED\n");
    uint8_t* data_ = reinterpret_cast<uint8_t*>(data);
    if (!is_write) {
        *data_ = 0;
    }
}

void handlerChipSelectUnit(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 2);
    assert(count == 1);
    // figure out which chip select unit is being accessed
    uint16_t unitIndex = (addr & 0x0078) >> 3;
    uint16_t registerIndex = (addr & 0x0007) >> 1;
    uint16_t* data_ = reinterpret_cast<uint16_t*>(data);

    ChipSelectUnit& unit = chipSelectUnits[unitIndex];
    switch (registerIndex) {
        case 0:
            if (is_write)
                unit.addressLowRegister = *data_;
            else
                *data_ = unit.addressLowRegister;
            break;
        case 1:
            if (is_write)
                unit.addressHighRegister = *data_;
            else
                *data_ = unit.addressHighRegister;
            break;
        case 2:
            if (is_write)
                unit.addressMaskLowRegister = *data_;
            else
                *data_ = unit.addressMaskLowRegister;
            break;
        case 3:
            if (is_write)
                unit.addressMaskHighRegister = *data_;
            else
                *data_ = unit.addressMaskHighRegister;
            break;
    }
    unit.Debug(std::to_string(unitIndex));
}

void handlerUnhandled(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    fprintf(stderr, "unhandled io exit: %s port:%04x size:%zu count:%zu ",
            is_write ? "write" : "read", addr, length, count);
    if (is_write && count == 1) {
        if (length == 1)
            fprintf(stderr, "data:%02x\n", *reinterpret_cast<uint8_t*>(data));
        else if (length == 2)
            fprintf(stderr, "data:%04x\n", *reinterpret_cast<uint16_t*>(data));
        else if (length == 4)
            fprintf(stderr, "data:%08x\n", *reinterpret_cast<uint32_t*>(data));
        else if (length == 8)
            fprintf(stderr, "data:%16lx\n", *reinterpret_cast<uint64_t*>(data));
    } else {
        fprintf(stderr, "\n");
        memset(data, 0, length);
    }
}

void handlerLCD(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    fprintf(stderr, "LCD ACCESSED\n");
}

void handlerProductCode(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    if (!is_write) {
        *reinterpret_cast<uint8_t*>(data) = 0x01;
    }
}

void handlerOptionCode(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    if (!is_write) {
        *reinterpret_cast<uint8_t*>(data) = 0x00;
    }
}

void handlerJumperRegister(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    if (!is_write) {
        // jumper 3 & 4 installed
        printf("REQUESTED JUMPER VALUES (PLD)\n");
        *reinterpret_cast<uint8_t*>(data) = 0x02;
    }
}

void handlerManufacturerSpecific(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);
    if (!is_write) {
        *reinterpret_cast<uint8_t*>(data) = 0x00;
    }
}

static uint8_t a20register = 0;

void handlerA20Gate(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    uint8_t *data_ = reinterpret_cast<uint8_t*>(data);
    if (is_write) {
        // jumper 3 & 4 installed
        a20register = *data_;
        //printf("LOADED FAST A20 GATE REGISTER = %02x\n", a20register);
    } else {
        *data_ = a20register;
    }
}

void handlerPort1Pin(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    if (!is_write) {
        printf("REQUESTED JUMPER VALUES (386 PORT1)\n", a20register);
        *reinterpret_cast<uint8_t*>(data) = 0x80;
    }
}

std::map<AddressRange, std::shared_ptr<DevicePio>> pioDeviceTable;

std::map<AddressRange, io_handler_t> ioHandlerTable = {
    { { 0x60,   0x05 }, handlerKeyboard },
    { { 0x70,   0x02 }, handlerRtc },
    { { 0x72,   0x02 }, handlerLCD },
    { { 0x74,   0x01 }, handlerProductCode },
    { { 0x75,   0x01 }, handlerOptionCode },
    { { 0x77,   0x01 }, handlerJumperRegister },
    { { 0x80,   0x01 }, handlerPOSTCode },
    { { 0x92,   0x01 }, handlerA20Gate },
    { { 0x198,  0x08 }, handlerManufacturerSpecific },
    { { 0xF400, 0x40 }, handlerChipSelectUnit },
    { { 0xF834, 0x01 }, handlerTimerConfiguration },
    { { 0xF860, 0x01 }, handlerPort1Pin },
};

int main (int argc, char** argv) {
    assert(PAGE_SIZE == getpagesize());

    // open the kvm handle
    int kvmFd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmFd == -1) {
        perror("unable to open the kvm endpoint.");
        return EXIT_FAILURE;
    }

    // check for a if the KVM API is new enough (== 12)
    int ret = ioctl(kvmFd, KVM_GET_API_VERSION, NULL);
    if (ret == -1) {
        perror("KVM_GET_API_VERSION");
        return EXIT_FAILURE;
    } else if (ret != 12) {
        fprintf(stderr, "KVM_GET_API_VERSION %d, expected 12\n", ret);
        return EXIT_FAILURE;
    }

    // check for required extensions
    ret = ioctl(kvmFd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    if (ret == -1) {
        perror("KVM_CHECK_EXTENSION");
        return EXIT_FAILURE;
    } else if (!ret) {
        fprintf(stderr, "Required extension KVM_CAP_USER_MEMORY not available.\n");
        return EXIT_FAILURE;
    }

    // create a virtual machine handle
    int vmFd = ioctl(kvmFd, KVM_CREATE_VM, (unsigned long) 0);
    if (vmFd == -1) {
        perror("KVM_CREATE_VM");
        return EXIT_FAILURE;
    }

    // ----------------------- MEMORY MAP CREATION ----------------------------------
    // open rom image
    int romFd = open("roms/flash.bin", O_RDONLY | O_CLOEXEC);
    //int romFd = open("roms/flash2.bin", O_RDONLY | O_CLOEXEC);
    if (romFd == -1) {
        perror("unable to open the rom.");
        return EXIT_FAILURE;
    }

    // mmap memory to back the flash chip
    uint8_t* flashMemory = (uint8_t*)
            mmap(NULL, 0x80000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1 , 0);
    if (flashMemory == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page.");
        return EXIT_FAILURE;
    }
    if (read(romFd, flashMemory, 0x80000) != 0x80000) {
        fprintf(stderr, "failed to read 512 KiB from ROM file.\n");
        return 0;
    }
    close(romFd);

    // mmap memory to back the RAM
    uint8_t* ram = (uint8_t*)
            mmap(NULL, 0xA0000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ram == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page for lowmem.");
        return EXIT_FAILURE;
    }

    struct kvm_userspace_memory_region regionRam = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = 0xA0000,
        .userspace_addr = (uint64_t) ram
    };

    struct kvm_userspace_memory_region regionRomDos = {
        .slot = 1,
        .guest_phys_addr = 0xE0000,
        .memory_size = 0x10000,
        .userspace_addr = (uint64_t) flashMemory + 0x60000
    };

    struct kvm_userspace_memory_region regionBios = {
        .slot = 2,
        .guest_phys_addr = 0xF0000,
        .memory_size = 0x10000,
        .userspace_addr = (uint64_t) flashMemory + 0x70000
    };

    // this simulates the address wrap around when a20 is disabled
    struct kvm_userspace_memory_region regionRamWrap = {
        .slot = 3,
        .guest_phys_addr = 0x100000,
        .memory_size = 0x10000,
        .userspace_addr = (uint64_t) ram
    };

    struct kvm_userspace_memory_region regionRamWrapDisabled = {
        .slot = 3,
        .memory_size = 0
    };

    struct kvm_userspace_memory_region regionFlash = {
        .slot = 4,
        .guest_phys_addr = 0x03400000,
        .memory_size = 0x80000,
        .userspace_addr = (uint64_t) flashMemory
    };

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRam);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRomDos);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionBios);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrap);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlash);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }
    // -----------------------------------------------------------------------------

    ret = ioctl(vmFd, KVM_CREATE_IRQCHIP);
    if (ret == -1) {
        perror("KVM_CREATE_IRQCHIP");
        return EXIT_FAILURE;
    }

    struct kvm_pit_config configurationPIT = { .flags = KVM_PIT_SPEAKER_DUMMY };
    ret = ioctl(vmFd, KVM_CREATE_PIT2, &configurationPIT);
    if (ret == -1) {
        perror("KVM_CREATE_PIT2");
        return EXIT_FAILURE;
    }

    /*int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd == -1) {
        perror("eventfd");
        return EXIT_FAILURE;
    }

    struct kvm_irqfd irqfd { .fd = (__u32) efd, .gsi = 8, .flags = 0, .resamplefd = 0 };
    ret = ioctl(vmFd, KVM_IRQFD, &irqfd);
    if (ret == -1) {
        perror("KVM_IRQFD");
        return EXIT_FAILURE;
    }*/

    // create a virtual cpu for the vm
    int vcpuFd = ioctl(vmFd, KVM_CREATE_VCPU, (unsigned long) 0);
    if (vcpuFd == -1) {
        perror("Failed to create virtual CPU.");
        return EXIT_FAILURE;
    }

    ret = ioctl(kvmFd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (ret == -1) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        return EXIT_FAILURE;
    }

    struct kvm_run* vcpuRun = (struct kvm_run*)
            mmap(NULL, ret, PROT_READ | PROT_WRITE, MAP_SHARED, vcpuFd, 0);
    if (vcpuRun == (struct kvm_run*) -1) {
        perror("Unable to map vcpu run structure.");
        return EXIT_FAILURE;
    }

    struct kvm_coalesced_mmio_ring* ring = NULL;
    ret = ioctl(kvmFd, KVM_CHECK_EXTENSION, KVM_CAP_COALESCED_MMIO);
    if (ret == -1) {
        perror("KVM_CHECK_EXTENSION");
    } else if (ret) {
        ring = (struct kvm_coalesced_mmio_ring*) ((uint8_t *) vcpuRun + (ret * PAGE_SIZE));
    }

    // setup initial CPU state (real mode, base will put our usage in the flash chip)
    struct kvm_sregs sregs;
    memset(&sregs, 0, sizeof sregs);
    ret = ioctl(vcpuFd, KVM_GET_SREGS, &sregs);
    if (ret == -1) {
        perror("KVM_GET_SREGS");
        return EXIT_FAILURE;
    }

    sregs.cr0 = 0;
    sregs.cs.base = 0x000FFFF0;
    sregs.cs.selector = 0xFFFF;
    ret = ioctl(vcpuFd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
        perror("KVM_SET_SREGS");
        return EXIT_FAILURE;
    }

    struct kvm_regs regs = {
        .rax = 2,
        .rbx = 2,
        .rip = 0,
        .rflags = 0x2
    };

    ret = ioctl(vcpuFd, KVM_SET_REGS, &regs);
    if (ret == -1) {
        perror("KVM_GET_REGS");
        return EXIT_FAILURE;
    }

//#ifndef NDEBUG
#if 0
    // enable single stepping
    struct kvm_guest_debug debug = {
        .control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP
    };

    ret = ioctl(vcpuFd, KVM_SET_GUEST_DEBUG, &debug);
    if (ret == -1) {
        perror("KVM_SET_GUEST_DEBUG");
        return EXIT_FAILURE;
    }
#endif /* NDEBUG */

    EventLoop deviceEventLoop;

    // -------------------- DEVICES ----------------------
    //auto timer0 = std::make_shared<ProgrammableIntervalTimer>();
    //pioDeviceTable.emplace(AddressRange{0x40, 0x04}, timer0);

    //std::vector<std::shared_ptr<Prescalable>> prescalableDevices = { timer0 };
    //auto prescaler = std::make_shared<i386EXClockPrescaler>(prescalableDevices);
    //pioDeviceTable.emplace(AddressRange{0xF804, 0x02}, prescaler);
    
    auto com1 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com1->start("/tmp/3100.com1.socket", vmFd, 4)) {
        return EXIT_FAILURE;
    }

    auto com2 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com2->start("/tmp/3100.com2.socket", vmFd, 3)) {
        return EXIT_FAILURE;
    }

    auto com3 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com3->start("/tmp/3100.com3.socket", vmFd, 4)) {
        return EXIT_FAILURE;
    }

    auto com4 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com4->start("/tmp/3100.com4.socket", vmFd, 3)) {
        return EXIT_FAILURE;
    }

    pioDeviceTable.emplace(AddressRange{0x3f8, 0x08}, com1);
    pioDeviceTable.emplace(AddressRange{0x2f8, 0x08}, com2);
    pioDeviceTable.emplace(AddressRange{0x3e8, 0x08}, com3);
    pioDeviceTable.emplace(AddressRange{0x2e8, 0x08}, com4);

//#ifndef NDEBUG
#if 0
    // setup a disassembly library
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_REAL_16, ZYDIS_ADDRESS_WIDTH_16);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
#endif /* NDEBUG */

    // run until halt instruction is found
    bool previousWasDebug = false;
    uint8_t lastA20Register = a20register;
    while (1) {
        ret = ioctl(vcpuFd, KVM_RUN, NULL);
        if (ret == -1) {
            perror("KVM_RUN");
            return EXIT_FAILURE;
        }

//#ifndef NDEBUG
#if 0
        // get the register state
        memset(&regs, 0, sizeof regs);
        memset(&sregs, 0, sizeof sregs);
        ioctl(vcpuFd, KVM_GET_REGS, &regs);
        ioctl(vcpuFd, KVM_GET_SREGS, &sregs);

        if (!previousWasDebug || vcpuRun->exit_reason == KVM_EXIT_DEBUG) {
            // get the instruction state
            ZyanUSize ip = regs.rip & 0xFFFF;
            ZyanUSize offset = 0;
            ZyanUSize length = 0;
            void* codeBuffer = NULL;
            if (sregs.cs.base < 0x70000) {
                codeBuffer = ram;
                offset = (sregs.cs.base + ip) - 0x70000;
                length = 0x70000 - offset;
            } else if (sregs.cs.base < 0xF0000) {
                codeBuffer = flashMemory + 0x60000;
                offset = ip;
                length = 0x10000 - ip;
            } else if (sregs.cs.base < 0x100000) {
                codeBuffer = flashMemory + 0x70000;
                offset = ip;
                length = 0x10000 - ip;
            } else {
                codeBuffer = flashMemory;
                offset = (sregs.cs.base + ip) - 0x3400000;
                length = 0x80000 - offset;
            }

            ZydisDecodedInstruction instruction;
            int count = 1;
            printf("--- next instructions ---\n");
            while (count--
                    && ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, codeBuffer + offset, length, &instruction))) {
                printf("[%08llx:%04lx]  ", sregs.cs.base, ip);
                char buffer[256];
                ZydisFormatterFormatInstruction(&formatter, &instruction, buffer, sizeof buffer, regs.rip);
                puts(buffer);
                offset += instruction.length;
                ip += instruction.length;
                length -= instruction.length;
            }
            printf("--- ----------------- ---\n");
        } else {
            previousWasDebug = false;
        }
#endif /* NDEBUG */

        switch (vcpuRun->exit_reason) {
            case KVM_EXIT_HLT:
                puts("KVM_EXIT_HLT");
                return EXIT_SUCCESS;

            case KVM_EXIT_DEBUG:
                previousWasDebug = true;
                break;

            case KVM_EXIT_IO:
            {
                auto device = pioDeviceTable.find(vcpuRun->io.port);
                if (device != pioDeviceTable.end()) {
                    // new device pio structure
                    assert(vcpuRun->io.count == 1);
                    device->second->performKVMExitOperation(
                            vcpuRun->io.direction == KVM_EXIT_IO_OUT,
                            vcpuRun->io.port,
                            ((char *) vcpuRun) + vcpuRun->io.data_offset,
                            vcpuRun->io.size);
                } else {
                    // old static handler structure
                    io_handler_t handlerFunc = handlerUnhandled;
                    auto handler = ioHandlerTable.find(vcpuRun->io.port);
                    if (handler != ioHandlerTable.end()) {
                        handlerFunc = handler->second;
                    }
                    handlerFunc(vcpuRun->io.direction == KVM_EXIT_IO_OUT,
                            vcpuRun->io.port,
                            ((char *) vcpuRun) + vcpuRun->io.data_offset,
                            vcpuRun->io.size,
                            vcpuRun->io.count);
                }

                // check if the a20 register was changed
                if (lastA20Register != a20register) {
                    if (a20register) {
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrapDisabled);
                        if (ret == -1) {
                            perror("KVM_SET_USER_MEMORY_REGION");
                            return EXIT_FAILURE;
                        }
                    } else {
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrap);
                        if (ret == -1) {
                            perror("KVM_SET_USER_MEMORY_REGION");
                            return EXIT_FAILURE;
                        }
                    }
                    lastA20Register = a20register;
                }
            }
            break;

            case KVM_EXIT_MMIO:
                // handle coalesce ring
                /*if (ring) {
                    while (ring->first != ring->last) {
                        struct kvm_coalesced_mmio* m = &ring->coalesced_mmio[ring->first];
                        // do with w aht you'd do outside
                        fprintf(stderr, "unhandled coalesced KVM_EXIT_MMIO.\n");
                        ring->first = (ring->first + 1) % KVM_COALESCED_MMIO_MAX;
                    }   
                }*/
                /*fprintf(stderr, "unhandled mmio exit: %s addr:%016llx length:%d ",
                    vcpuRun->mmio.is_write ? "write" : "read",
                    vcpuRun->mmio.phys_addr,
                    vcpuRun->mmio.len);*/
                if (vcpuRun->mmio.is_write) {
                    /*if (vcpuRun->mmio.len == 1)
                        fprintf(stderr, "data:%02x\n", *((uint8_t*) vcpuRun->mmio.data));
                    else if (vcpuRun->mmio.len == 2)
                        fprintf(stderr, "data:%04x\n", *((uint16_t*) vcpuRun->mmio.data));
                    else if (vcpuRun->mmio.len == 4)
                        fprintf(stderr, "data:%08x\n", *((uint32_t*) vcpuRun->mmio.data));
                    else if (vcpuRun->mmio.len == 8)
                        fprintf(stderr, "data:%16lx\n", *((uint64_t*) vcpuRun->mmio.data));*/
                } else {
                    //fprintf(stderr, "\n");
                    memset(vcpuRun->mmio.data, 0, sizeof vcpuRun->mmio.data);
                }
                break;

            default:
                fprintf(stderr, "unhandled exit: %u\n", vcpuRun->exit_reason);
                return EXIT_FAILURE;
        }

        // handle coalesce ring
        /*if (ring) {
            while (ring->first != ring->last) {
                struct kvm_coalesced_mmio* m = &ring->coalesced_mmio[ring->first];
                // do with w aht you'd do outside
                fprintf(stderr, "unhandled coalesced KVM_EXIT_MMIO.\n");
                ring->first = (ring->first + 1) % KVM_COALESCED_MMIO_MAX;
            }   
        }*/
    }
    return EXIT_SUCCESS;
}