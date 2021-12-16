#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "AddressRange.hpp"
#include "hardware/ChipSelectUnit.hpp"
#include "hardware/Timer.hpp"
#include "hardware/i386EXClockPrescaler.hpp"
#include "hardware/Serial.hpp"
#include "hardware/HexDisplay.hpp"
#include "hardware/DS12887.hpp"

//#define HIGH_MEMORY_SIZE (0x100000)
#define HIGH_MEMORY_SIZE (0)
#define LOW_MEMORY_SIZE (0x70000)

#ifdef DISASSEMBLE
#include <Zydis/Zydis.h>
#endif

#define PAGE_SIZE 4096

typedef void (*io_handler_t)(bool is_write, uint16_t addr, void* data, size_t length, size_t count);

sig_atomic_t requestExit = 0;

void sigintHandler(int signo)
{
    requestExit = 1;
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
        fprintf(stderr, "REQUESTED JUMPER VALUES (PLD)\n");
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

// default register state on 386EX is enabled
static uint8_t a20register = 2;

void handlerA20Gate(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    uint8_t *data_ = reinterpret_cast<uint8_t*>(data);
    if (is_write) {
        a20register = *data_;
#if !(defined NDEBUG)
        fprintf(stderr, "LOADED FAST A20 GATE REGISTER = %02x\n", a20register);
#endif
    } else {
        *data_ = a20register;
    }
}

void handlerPort1Pin(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    if (!is_write) {
        fprintf(stderr, "REQUESTED JUMPER VALUES (386 PORT1)\n", a20register);
        *reinterpret_cast<uint8_t*>(data) = 0x80;
    }
}

void handlerPort3Pin(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(length == 1);
    assert(count == 1);

    if (!is_write) {
        fprintf(stderr, "REQUESTED JUMPER VALUES (386 PORT3)\n", a20register);
        *reinterpret_cast<uint8_t*>(data) = 0x04;
    }
}

enum class FlashState {
    Read,
    CommandByte_1,
    CommandByte_2,
    CommandByte_3,
    CommandByte_4,
    CommandByte_5,
    Program,
    ProductIdentification,
    SectorErase,
};

FlashState flashState = FlashState::Read;

#if (defined VIRTUAL_DISK)
// geometry for a (512 byte/sector * 63 sectors/track * 255 heads (track/cylinder) * 8 cylinders)
//   = 64260 KiB disk
//constexpr uint8_t numberDiskHeads = 255;
//constexpr uint8_t numberDiskSectors = 63;
//constexpr uint8_t numberDiskCylinders = 8;
uint32_t selectedDiskLBA = 0;
bool updateDiskMapping = false;

void handlerDiskRegisters(bool is_write, uint16_t addr, void* data, size_t length, size_t count) {
    assert(count == 1);

    if (is_write) {
        if ((addr & 0x07) < 4) {
            // write is to the (4K) LBA register
            memcpy((void*) (reinterpret_cast<uint8_t*>(&selectedDiskLBA) + (addr & 0x03)), data,
                    length);
        } else {
            // write is to the update mapping register
            updateDiskMapping = true;
        }
    } else {
        memcpy(data, (void*) (reinterpret_cast<uint8_t*>(&selectedDiskLBA) + (addr & 0x03)),
                length);
    }
}
#endif

std::map<AddressRange, io_handler_t> ioHandlerTable = {
    { { 0x60,   0x05 }, handlerKeyboard },
    { { 0x72,   0x02 }, handlerLCD },
    { { 0x74,   0x01 }, handlerProductCode },
    { { 0x75,   0x01 }, handlerOptionCode },
    { { 0x77,   0x01 }, handlerJumperRegister },
    { { 0x80,   0x01 }, handlerPOSTCode },
    { { 0x92,   0x01 }, handlerA20Gate },
    { { 0x198,  0x08 }, handlerManufacturerSpecific },
#if (defined VIRTUAL_DISK)
    { { 0xD000, 0x08 }, handlerDiskRegisters },
#endif
    { { 0xF834, 0x01 }, handlerTimerConfiguration },
    { { 0xF860, 0x01 }, handlerPort1Pin },
    { { 0xF870, 0x01 }, handlerPort3Pin },
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
    int romFd = open("roms/flash.bin", O_RDWR | O_CLOEXEC);
    if (romFd == -1) {
        perror("unable to open the rom.");
        return EXIT_FAILURE;
    }

    // mmap memory to back the flash chip
    uint8_t* flashMemory = (uint8_t*)
            mmap(NULL, 0x80000, PROT_READ | PROT_WRITE, MAP_SHARED, romFd, 0);
    if (flashMemory == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page.");
        return EXIT_FAILURE;
    }
    close(romFd);

#if (defined VIRTUAL_DISK)
    // open disk option rom
    int optionFd = open("roms/virtual-disk/option.rom", O_RDONLY | O_CLOEXEC);
    if (optionFd == -1) {
        perror("unable to open option rom.");
        return EXIT_FAILURE;
    }

    // TODO: currently I'm actually using this region for variables inside the option
    //       rom. need to switch to a low-mem stealing approach.
    /*uint8_t* optionRom = (uint8_t*)
            mmap(NULL, 0x2000, PROT_READ, MAP_SHARED, optionFd, 0);
    if (optionRom == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page.");
        return EXIT_FAILURE;
    }
    close(optionFd);*/

    uint8_t* optionRom = (uint8_t*)
            mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (optionRom == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page.");
        return EXIT_FAILURE;
    }
    if (read(optionFd, optionRom, 0x2000) != 0x2000) {
        fprintf(stderr, "failed to read 8 KiB from option rom file.\n");
        return EXIT_FAILURE;
    }
    close(optionFd);

    // open disk image
    int diskFd = open("roms/drivec.img", O_RDWR | O_CLOEXEC);
    if (diskFd == -1) {
        perror("Unable to open disk image.");
        return EXIT_FAILURE;
    }

    uint8_t* diskData = nullptr;
#endif

    // mmap memory to back the RAM
    uint8_t* ram = (uint8_t*) mmap(NULL, LOW_MEMORY_SIZE + HIGH_MEMORY_SIZE, 
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ram == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page for lowmem.");
        return EXIT_FAILURE;
    }

#if 0
    int vgaFd = open("roms/vga.bin", O_RDONLY | O_CLOEXEC);
    if (vgaFd == -1) {
        perror("unable to open vga rom.");
        return EXIT_FAILURE;
    }

    uint8_t* vgaRom = (uint8_t*)
            mmap(NULL, 0x8000, PROT_READ, MAP_SHARED, vgaFd, 0);
    if (vgaRom == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page.");
        return EXIT_FAILURE;
    }
    close(vgaFd);
#endif

    struct kvm_userspace_memory_region regionRam = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = LOW_MEMORY_SIZE,
        .userspace_addr = (uint64_t) ram
    };

    struct kvm_userspace_memory_region regionRomDos = {
        .slot = 1,
        .flags = KVM_MEM_READONLY,
        .guest_phys_addr = 0xE0000,
        .memory_size = 0x10000,
        .userspace_addr = (uint64_t) flashMemory + 0x60000
    };

    struct kvm_userspace_memory_region regionBios = {
        .slot = 2,
        .flags = KVM_MEM_READONLY,
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

#if (HIGH_MEMORY_SIZE)
    struct kvm_userspace_memory_region regionRamWrapHighMem = {
        .slot = 3,
        .guest_phys_addr = 0x100000,
        .memory_size = HIGH_MEMORY_SIZE,
        .userspace_addr = (uint64_t) ram + LOW_MEMORY_SIZE
    };
#endif

    struct kvm_userspace_memory_region regionFlash = {
        .slot = 4,
        .flags = KVM_MEM_READONLY,
        .guest_phys_addr = 0x03400000,
        .memory_size = 0x80000,
        .userspace_addr = (uint64_t) flashMemory
    };

    struct kvm_userspace_memory_region regionFlashAlias = {
        .slot = 5,
        .flags = KVM_MEM_READONLY,
        .guest_phys_addr = 0x03480000,
        .memory_size = 0x80000,
        .userspace_addr = (uint64_t) flashMemory
    };

    struct kvm_userspace_memory_region regionFlash_Unmap = {
        .slot = 4,
        .memory_size = 0
    };

    struct kvm_userspace_memory_region regionFlashAlias_Unmap = {
        .slot = 5,
        .memory_size = 0
    };

#if (defined VIRTUAL_DISK)
    struct kvm_userspace_memory_region regionOptionRom = {
        .slot = 6,
        // TODO: we actually write to this region for each of use purposes
        //.flags = KVM_MEM_READONLY,
        .guest_phys_addr = 0xC8000,
        .memory_size = 0x1000,
        .userspace_addr = (uint64_t) optionRom
    };

    struct kvm_userspace_memory_region regionOptionRom_Boot = {
        .slot = 7,
        .flags = KVM_MEM_READONLY,
        .guest_phys_addr = 0xC9000,
        .memory_size = 0x1000,
        .userspace_addr = (uint64_t) optionRom + 0x1000
    };

    struct kvm_userspace_memory_region regionOptionRom_Disk = {
        .slot = 7,
        .guest_phys_addr = 0xC9000,
        .memory_size = 0x1000,
        .userspace_addr = 0
    };

    struct kvm_userspace_memory_region regionOptionRom_Unmap = {
        .slot = 7,
        .memory_size = 0
    };
#endif

#if 0
    struct kvm_userspace_memory_region regionVGARom = {
        .slot = 8,
        .guest_phys_addr = 0xC0000,
        .memory_size = 0x8000,
        .userspace_addr = (uint64_t) vgaRom
    };
#endif

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

#if HIGH_MEMORY_SIZE
    // wrap is disabled (a20 line enabled) by default on 386EX
    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrapHighMem);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }
#endif

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlash);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlashAlias);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

#if (defined VIRTUAL_DISK)
    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionOptionRom);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionOptionRom_Boot);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }
#endif

#if 0
    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionVGARom);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return EXIT_FAILURE;
    }
#endif
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
    sregs.cs.base = 0x03470000;
    sregs.cs.selector = 0xF000;
    ret = ioctl(vcpuFd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
        perror("KVM_SET_SREGS");
        return EXIT_FAILURE;
    }

    struct kvm_regs regs = { .rip = 0x0000FFF0 };
    ret = ioctl(vcpuFd, KVM_SET_REGS, &regs);
    if (ret == -1) {
        perror("KVM_GET_REGS");
        return EXIT_FAILURE;
    }

#ifdef DISASSEMBLE
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
    std::map<AddressRange, std::shared_ptr<DevicePio>> pioDeviceTable;

    //auto timer0 = std::make_shared<ProgrammableIntervalTimer>();
    //pioDeviceTable.emplace(AddressRange{0x40, 0x04}, timer0);

    // virtual device: 386EX prescaler unit
    std::vector<std::shared_ptr<Prescalable>> prescalableDevices = { };
    auto prescaler = std::make_shared<i386EXClockPrescaler>(prescalableDevices);
    pioDeviceTable.emplace(AddressRange{0xF804, 0x02}, prescaler);
    
    // virtual device: COM1
    auto com1 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com1->start("/tmp/3100.com1.socket", vmFd, 4)) {
        return EXIT_FAILURE;
    }
    pioDeviceTable.emplace(AddressRange{0x03f8, 0x08}, com1);

    // virtual device: COM2
    auto com2 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com2->start("/tmp/3100.com2.socket", vmFd, 3)) {
        return EXIT_FAILURE;
    }
    pioDeviceTable.emplace(AddressRange{0x02f8, 0x08}, com2);

    // virtual device: COM3
    auto com3 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com3->start("/tmp/3100.com3.socket", vmFd, 4)) {
        return EXIT_FAILURE;
    }
    pioDeviceTable.emplace(AddressRange{0x03e8, 0x08}, com3);

    // virtual device: COM4
    auto com4 = std::make_shared<Serial16450>(deviceEventLoop);
    if (!com4->start("/tmp/3100.com4.socket", vmFd, 3)) {
        return EXIT_FAILURE;
    }
    pioDeviceTable.emplace(AddressRange{0x02e8, 0x08}, com4);

    // virtual device: Hex Display
    auto hexDisplay = std::make_shared<HexDisplay>();
    pioDeviceTable.emplace(AddressRange{0xe000, 0x08}, hexDisplay);

    // virtual device: Chip Select Units (unit 7 "upper chip select" has special starting values)
    std::shared_ptr<ChipSelectUnit> csus[8];
    for (int i = 0; i < 7; i++) {
        csus[i] = std::make_shared<ChipSelectUnit>();
    }
    csus[7] = std::make_shared<ChipSelectUnit>(0xFFFF, 0xFF6F, 0xFFFF, 0xFFFF);
    for (uint16_t csusBaseAddress = 0xF400, i = 0; i < 8; csusBaseAddress += 0x08, i++) {
        pioDeviceTable.emplace(AddressRange{csusBaseAddress, 0x08}, csus[i]);
    }

    // virtual device: RTC
    auto rtc = std::make_shared<DS12887>();
    pioDeviceTable.emplace(AddressRange{0x70, 0x02}, rtc);

#ifdef DISASSEMBLE
    // setup a disassembly library
    ZydisDecoder decoderReal, decoderP16, decoderP32;
    ZydisDecoderInit(&decoderReal, ZYDIS_MACHINE_MODE_REAL_16,   ZYDIS_ADDRESS_WIDTH_16);
    ZydisDecoderInit(&decoderP16,  ZYDIS_MACHINE_MODE_LEGACY_16, ZYDIS_ADDRESS_WIDTH_16);
    ZydisDecoderInit(&decoderP32,  ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_ADDRESS_WIDTH_32);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
#endif /* NDEBUG */

    // signals
    signal(SIGINT, sigintHandler);

    // run until halt instruction is found
    bool previousWasDebug = false;
    uint8_t lastA20Register = a20register;
    while (!requestExit) {
        ret = ioctl(vcpuFd, KVM_RUN, NULL);
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                fprintf(stderr, "internal error occurred: %s\n", strerror(errno));
                break;
            }
        }

#ifdef DISASSEMBLE
        // get the register state
        memset(&regs, 0, sizeof regs);
        memset(&sregs, 0, sizeof sregs);
        ioctl(vcpuFd, KVM_GET_REGS, &regs);
        ioctl(vcpuFd, KVM_GET_SREGS, &sregs);

        if (!previousWasDebug || vcpuRun->exit_reason == KVM_EXIT_DEBUG) {
            // figure out the memory region the instruction pointer is in
            ZyanUSize ip = sregs.cs.base + regs.rip;
            ZyanUSize offset = 0;
            ZyanUSize length = 0;
            uint8_t* codeBuffer = NULL;
            if (ip < 0xA0000) {
                codeBuffer = ram;
                offset = ip;
                length = 0xA0000 - offset;
            }
#if 0
            else if (ip >= 0xC0000 && ip < 0xC8000) {
                codeBuffer = vgaRom;
                offset = ip & 0x7FFF;
                length = 0x8000 - ip;
            }
#endif
            else if (ip >= 0xC8000 && ip < 0xCA000) {
                codeBuffer = optionRom;
                offset = ip & 0x1FFF;
                length = 0x2000 - ip;
            } else if (ip >= 0xE0000 && ip < 0xF0000) {
                codeBuffer = flashMemory + 0x60000;
                offset = ip & 0xFFFF;
                length = 0x10000 - ip;
            } else if (ip >= 0xF0000 && ip < 0x100000) {
                codeBuffer = flashMemory + 0x70000;
                offset = ip & 0xFFFF;
                length = 0x10000 - ip;
            } else if (ip >= 0x3400000 && ip <= 0x34FFFFF) {
                codeBuffer = flashMemory;
                offset = ip & 0x7FFFF;
                length = 0x80000 - offset;
            }

            ZydisDecodedInstruction instruction;
            int count = 8;
            fprintf(stderr, "--- next instructions ---\n");
            while (count--) {
                if (sregs.cr0 & 1) {
                    // protected mode
                    if (sregs.cs.db) {
                        // 32-bit protected mode
                        if (!ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoderP32, codeBuffer + offset, length, &instruction))) {
                            break;
                        }
                        fprintf(stderr, "[%08llx:%08llx]  ", sregs.cs.base, ip - sregs.cs.base);
                    } else {
                        // 16-bit protected mode
                        if (!ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoderP16, codeBuffer + offset, length, &instruction))) {
                            break;
                        }
                        fprintf(stderr, "[%08llx:%04llx]  ", sregs.cs.base, ip - sregs.cs.base);
                    }
                } else {
                    // real mode
                    if (!ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoderReal, codeBuffer + offset, length, &instruction))) {
                        break;
                    }
                    fprintf(stderr, "[%08llx:%04llx]  ", sregs.cs.base, ip - sregs.cs.base);
                }

                char buffer[256];
                ZydisFormatterFormatInstruction(&formatter, &instruction, buffer, sizeof buffer, regs.rip);
                fprintf(stderr, "%s\n", buffer);
                offset += instruction.length;
                ip += instruction.length;
                length -= instruction.length;
            }
            fprintf(stderr, "--- ----------------- ---\n");
        } else {
            previousWasDebug = false;
        }
#endif /* NDEBUG */

        switch (vcpuRun->exit_reason) {
            case KVM_EXIT_HLT:
                fprintf(stderr, "halt instruction executed\n");
                requestExit = 1;
                break;

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
                if ((lastA20Register & 2) != (a20register & 2)) {
#if (HIGH_MEMORY_SIZE)
                    // if we have high mem, we have to unmap memory before selecting upper memory
                    // or wrapping the lower memory
                    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrapDisabled);
                    if (ret == -1) {
                        perror("KVM_SET_USER_MEMORY_REGION");
                        return EXIT_FAILURE;
                    }
#endif
                    if (a20register & 2) {
#if (HIGH_MEMORY_SIZE)
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrapHighMem);
#else
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionRamWrapDisabled);
#endif
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

#if (defined VIRTUAL_DISK)
                // check if the disk registers were changed
                if (updateDiskMapping) {
                    // unmap existing data
                    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionOptionRom_Unmap);
                    if (ret == -1) {
                        perror("KVM_SET_USER_MEMORY_REGION (unmap disk)");
                        return EXIT_FAILURE;
                    }

                    if (munmap(diskData, PAGE_SIZE) == -1) {
                        perror("failed to unmap old disk data");
                        return EXIT_FAILURE;
                    }

                    // map a new region
                    diskData = (uint8_t*) mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED, diskFd, (off_t) selectedDiskLBA * 512);
                    if (diskData == (uint8_t*) -1) {
                        perror("failed to mmap disk data");
                        return EXIT_FAILURE;
                    }

                    // first sector
                    /*for (int i = 0; i < 4096; i += 8) {
                        fprintf(stderr, "0x%08x: ", (selectedDiskLBA * 512) + i);
                        for (int j = 0; j < 8; j++) {
                            fprintf(stderr, "0x%02x ", diskData[i + j]);
                        }
                        fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "\n");*/

                    // update mapping
                    regionOptionRom_Disk.userspace_addr = (uint64_t) diskData;
                    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionOptionRom_Disk);
                    if (ret == -1) {
                        perror("KVM_SET_USER_MEMORY_REGION (map disk)");
                        return EXIT_FAILURE;
                    }
                    updateDiskMapping = false;
                    fprintf(stderr, "virtual disk: LBA mapped: %08x\n", selectedDiskLBA);
                }
#endif
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
#if (defined VIRTUAL_DISK)
                // the flash disk is being accessed
                if (vcpuRun->mmio.phys_addr >= 0x3400000 && vcpuRun->mmio.phys_addr < 0x350000) {
                    off_t offset = vcpuRun->mmio.phys_addr & 0x7FFFF;
                    bool unmapFlash = false;
                    bool mapFlash = false;

                    //fprintf(stderr, "flash disk: %s: addr:%016lx length:%d ", vcpuRun->mmio.is_write ? "write" : "read", offset, vcpuRun->mmio.len);
                    if (vcpuRun->mmio.is_write) {
                        // Writing data to the flash disk
                        assert(vcpuRun->mmio.len == 1);
                        uint8_t command = *reinterpret_cast<uint8_t*>(vcpuRun->mmio.data);
                        //fprintf(stderr, "data:%02x\n", command);
                        if (flashState == FlashState::Program) {
                            flashMemory[offset] = command;
                            flashState = FlashState::Read;
                        } else if (command == 0xF0) {
                            // reset device
                            flashState = FlashState::Read;
                        } else if (flashState == FlashState::Read && ((offset & 0x7FF) == 0x555) && command == 0xAA) {
                            flashState = FlashState::CommandByte_1;
                        } else if (flashState == FlashState::CommandByte_1 && ((offset & 0x7FF) == 0x2AA) && command == 0x55) {
                            flashState = FlashState::CommandByte_2;
                        } else if (flashState == FlashState::CommandByte_2 && ((offset & 0x7FF) == 0x555) && command == 0x80) {
                            flashState = FlashState::CommandByte_3;
                        } else if (flashState == FlashState::CommandByte_3 && ((offset & 0x7FF) == 0x555) && command == 0xAA) {
                            flashState = FlashState::CommandByte_4;
                        } else if (flashState == FlashState::CommandByte_4 && ((offset & 0x7FF) == 0x2AA) && command == 0x55) {
                            flashState = FlashState::CommandByte_5;
                        } else if (flashState == FlashState::CommandByte_2 && ((offset & 0x7FF) == 0x555) && command == 0xA0) {
                            flashState = FlashState::Program;
                        } else if (flashState == FlashState::CommandByte_2 && ((offset & 0x7FF) == 0x555) && command == 0x90) {
                            flashState = FlashState::ProductIdentification;
                            unmapFlash = true;
                            fprintf(stderr, "flash disk: detected product identification command.\n");
                        } else if (flashState == FlashState::CommandByte_5 && command == 0x30) {
                            // sector erase
                            memset(flashMemory + (offset & 0x70000), 0xff, 0x10000);
                            flashState = FlashState::Read;
                            fprintf(stderr, "flash disk: sector erased: %016lx\n", offset & 0x70000);
                        } else if (flashState == FlashState::CommandByte_5 && ((offset & 0x7FF) == 0x555) && command == 0x10) {
                            // chip erase
                            memset(flashMemory, 0xff, 0x80000);
                            flashState = FlashState::Read;
                            fprintf(stderr, "flash disk: chip erased\n");
                        } else {
                            fprintf(stderr, "flash disk: unknown command sequence.\n");
                            flashState = FlashState::Read;
                            return EXIT_FAILURE;
                        }
                    } else { 
                        // Reading data from the flash disk
                        //fprintf(stderr, "\n");
                        if (flashState == FlashState::ProductIdentification) {
                            mapFlash = true;
                            *reinterpret_cast<uint8_t*>(vcpuRun->mmio.data) =
                                    (offset & 1) ? 0xA4 : 0x01;
                            fprintf(stderr, "flash disk: product identification read.\n");
                        }
                        flashState = FlashState::Read;
                    }

                    if (unmapFlash) {
                        // unmap the flash memory (to control reads)
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlash_Unmap);
                        if (ret == -1) {
                            perror("KVM_SET_USER_MEMORY_REGION (unmap flash)");
                            return EXIT_FAILURE;
                        }
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlashAlias_Unmap);
                        if (ret == -1) {
                            perror("KVM_SET_USER_MEMORY_REGION (unmap flash alias)");
                            return EXIT_FAILURE;
                        }
                    } else if (mapFlash) {
                        // map the flash memory
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlash);
                        if (ret == -1) {
                            perror("KVM_SET_USER_MEMORY_REGION (map flash)");
                            return EXIT_FAILURE;
                        }
                        ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &regionFlashAlias);
                        if (ret == -1) {
                            perror("KVM_SET_USER_MEMORY_REGION (map flash alias)");
                            return EXIT_FAILURE;
                        }
                    }
                } else {
#endif
#if !(defined NDEBUG)
                fprintf(stderr, "unhandled mmio exit: %s addr:%016llx length:%d ",
                    vcpuRun->mmio.is_write ? "write" : "read",
                    vcpuRun->mmio.phys_addr,
                    vcpuRun->mmio.len);
#endif
                if (vcpuRun->mmio.is_write) {
#if !(defined NDEBUG)
                    if (vcpuRun->mmio.len == 1)
                        fprintf(stderr, "data:%02x\n", *((uint8_t*) vcpuRun->mmio.data));
                    else if (vcpuRun->mmio.len == 2)
                        fprintf(stderr, "data:%04x\n", *((uint16_t*) vcpuRun->mmio.data));
                    else if (vcpuRun->mmio.len == 4)
                        fprintf(stderr, "data:%08x\n", *((uint32_t*) vcpuRun->mmio.data));
                    else if (vcpuRun->mmio.len == 8)
                        fprintf(stderr, "data:%16lx\n", *((uint64_t*) vcpuRun->mmio.data));
#endif
                } else {
#if !(defined NDEBUG)
                    fprintf(stderr, "\n");
#endif
                    memset(vcpuRun->mmio.data, 0, sizeof vcpuRun->mmio.data);
                }
#if (defined VIRTUAL_DISK)
                }
#endif
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
