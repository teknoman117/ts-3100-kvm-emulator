#ifndef PRESCALABLE_HPP_
#define PRESCALABLE_HPP_

struct Prescalable {
    virtual ~Prescalable() = default;
    virtual void setPrescaler(uint16_t prescaler) = 0;
};

#endif /* PRESCALABLE_HPP_ */