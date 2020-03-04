#ifndef RANGE_HPP_
#define RANGE_HPP_

#include <cstddef>

struct AddressRange
{
    using size_type = size_t;
    size_type start;
    size_type length;

    bool operator<(const AddressRange& rhs) const {
        return (start + length - 1) < rhs.start;
    }

    constexpr AddressRange(size_type start)
        : start(start), length(1) {}
    
    constexpr AddressRange(size_type start, size_type length)
        : start(start), length(length) {}
};

#endif /* RANGE_HPP_ */