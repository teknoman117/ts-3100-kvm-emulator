#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <linux/kvm.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>

int main (int argc, char** argv) {
    // sample executable to run under kvm
    const uint8_t code[] = {
        0xba, 0xf8, 0x03, /* mov $0x3f8, %dx */
        0x00, 0xd8,       /* add %bl, %al */
        0x04, '0',        /* add $'0', %al */
        0xee,             /* out %al, (%dx) */
        0xb0, '\n',       /* mov $'\n', %al */
        0xee,             /* out %al, (%dx) */
        0xf4,             /* hlt */
    };

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

    // assign some memory for the code of the VM
    uint8_t* codePage = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1 , 0);
    if (codePage == (uint8_t*) -1) {
        perror("Unable to mmap an anonymous page.");
        return EXIT_FAILURE;
    }
    memcpy(codePage, code, sizeof code);

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x1000,
        .memory_size = 0x1000,
        .userspace_addr = (uint64_t) codePage
    };

    ret = ioctl(vmFd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
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

    struct kvm_run* vcpuRun = mmap(NULL, ret, PROT_READ | PROT_WRITE, MAP_SHARED, vcpuFd, 0);
    if (vcpuRun == (struct kvm_run*) -1) {
        perror("Unable to map vcpu run structure.");
        return EXIT_FAILURE;
    }

    // setup initial CPU state
    struct kvm_sregs sregs;
    memset(&sregs, 0, sizeof sregs);
    ret = ioctl(vcpuFd, KVM_GET_SREGS, &sregs);
    if (ret == -1) {
        perror("KVM_GET_SREGS");
        return EXIT_FAILURE;
    }

    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ret = ioctl(vcpuFd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
        perror("KVM_SET_SREGS");
        return EXIT_FAILURE;
    }

    struct kvm_regs regs = {
        .rip = 0x1000,
        .rax = 2,
        .rbx = 2,
        .rflags = 0x2
    };

    ret = ioctl(vcpuFd, KVM_SET_REGS, &regs);
    if (ret == -1) {
        perror("KVM_GET_REGS");
        return EXIT_FAILURE;
    }

    // enable single stepping
    struct kvm_guest_debug debug = {
        .control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP
    };

    ret = ioctl(vcpuFd, KVM_SET_GUEST_DEBUG, &debug);
    if (ret == -1) {
        perror("KVM_SET_GUEST_DEBUG");
        return EXIT_FAILURE;
    }

    // run until halt instruction is found
    while (1) {
        ret = ioctl(vcpuFd, KVM_RUN, NULL);
        if (ret == -1) {
            perror("KVM_RUN");
            return EXIT_FAILURE;
        }

        switch (vcpuRun->exit_reason) {
            case KVM_EXIT_HLT:
                puts("KVM_EXIT_HLT");
                return EXIT_SUCCESS;
            
            case KVM_EXIT_DEBUG:
                memset(&regs, 0, sizeof regs);
                ioctl(vcpuFd, KVM_GET_REGS, &regs);
                break;

            case KVM_EXIT_IO:
                if (vcpuRun->io.direction == KVM_EXIT_IO_OUT
                        && vcpuRun->io.size == 1
                        && vcpuRun->io.port == 0x3f8
                        && vcpuRun->io.count == 1)
                    putchar(*(((char *) vcpuRun) + vcpuRun->io.data_offset));
                else {
                    fprintf(stderr, "unhandled KVM_EXIT_IO.\n");
                    return EXIT_FAILURE;
                }
                break;

            default:
                fprintf(stderr, "unhandled exit: %u\n", vcpuRun->exit_reason);
                return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}