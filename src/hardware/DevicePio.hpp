#ifndef PIOOPERATIONS_HPP_
#define PIOOPERATIONS_HPP_

#include <stdexcept>

struct DevicePio {
    virtual ~DevicePio() = default;

    void performKVMExitOperation(bool is_write, uint16_t address, void* data, size_t length) {
        switch (length) {
            case 1:
            {
                uint8_t* data_ = reinterpret_cast<uint8_t*>(data);
                if (is_write)
                    iowrite8(address, *data_);
                else
                    *data_ = ioread8(address);
                break;
            }
            case 2:
            {
                uint16_t* data_ = reinterpret_cast<uint16_t*>(data);
                if (is_write)
                    iowrite16(address, *data_);
                else
                    *data_ = ioread16(address);
                break;
            }
            case 4:
            {
                uint32_t* data_ = reinterpret_cast<uint32_t*>(data);
                if (is_write)
                    iowrite32(address, *data_);
                else
                    *data_ = ioread32(address);
                break;
            }
            case 8:
            {
                uint64_t* data_ = reinterpret_cast<uint64_t*>(data);
                if (is_write)
                    iowrite64(address, *data_);
                else
                    *data_ = ioread64(address);
                break;
            }
            default:
                throw std::runtime_error("oddly sized operation length.");
        }
    }

    virtual void iowrite8(uint16_t address, uint8_t data) {
        throw std::runtime_error("iowrite8 is unimplemented for this device");
    }

    virtual void iowrite16(uint16_t address, uint16_t data) {
        throw std::runtime_error("iowrite16 is unimplemented for this device");
    }

    virtual void iowrite32(uint16_t address, uint32_t data) {
        throw std::runtime_error("iowrite32 is unimplemented for this device");
    }

    virtual void iowrite64(uint16_t address, uint64_t data) {
        throw std::runtime_error("iowrite64 is unimplemented for this device");
    }

    virtual uint8_t ioread8(uint16_t address) {
        throw std::runtime_error("ioread8 is unimplemented for this device");
        return 0;
    }

    virtual uint16_t ioread16(uint16_t address) {
        throw std::runtime_error("ioread16 is unimplemented for this device");
        return 0;
    }

    virtual uint32_t ioread32(uint16_t address) {
        throw std::runtime_error("ioread32 is unimplemented for this device");
        return 0;
    }

    virtual uint64_t ioread64(uint16_t address) {
        throw std::runtime_error("ioread64 is unimplemented for this device");
        return 0;
    }
};

#endif /* PIOOPERATIONS_HPP_ */