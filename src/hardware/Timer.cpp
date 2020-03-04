#include "Timer.hpp"

#include <cstdio>
#include <cstring>
#include <cassert>

ProgrammableIntervalTimer::ProgrammableIntervalTimer() : timerPrescaler(2), state{} {
    // initialize the last updated timer
    auto now = std::chrono::high_resolution_clock::now();
    state[0].lastRecord = now;
    state[1].lastRecord = now;
    state[2].lastRecord = now;
    state[0].waitingForLoad = true;
    state[1].waitingForLoad = true;
    state[2].waitingForLoad = true;
    state[0].accessMode = AccessMode::LowByteHighByte;
    state[1].accessMode = AccessMode::LowByteHighByte;
    state[2].accessMode = AccessMode::LowByteHighByte;
}

ProgrammableIntervalTimer::~ProgrammableIntervalTimer() {}

// prescaler register uses "divisor - 2", we store the adjusted one
void ProgrammableIntervalTimer::setPrescaler(uint16_t prescaler) {
    resolveTimers();
    timerPrescaler = prescaler;
}

void ProgrammableIntervalTimer::writeCommand(ChannelCommand command) {
    resolveTimers();
    if (command.standard.channel == 3) {
        // READBACK COMMAND
        for (int i = 0; i < 3; i++) {
            // skip channel if not selected
            if ((i == 0 && !command.readback.readChannel0)
                    || (i == 1 && !command.readback.readChannel1)
                    || (i == 2 && !command.readback.readChannel2))
                continue;
            
            // setup byte select state machine
            if (!command.readback.latchStatus) {
                state[i].accessByte = ByteSelect::StatusByte;
            } else {
                state[i].accessByte =
                        (state[i].accessMode == AccessMode::HighByteOnly)
                        ? ByteSelect::HighByte
                        : ByteSelect::LowByte;
            }

            // latch value if requested
            if (!command.readback.latchCount) {
                state[i].latch = state[i].value;
                state[i].latched = true;
            }
        }
    } else if (command.standard.accessMode == AccessMode::LatchCountValue) {
        // COUNTER LATCH COMMAND
        state[command.standard.channel].latch = state[command.standard.channel].value;
        state[command.standard.channel].latched = true;
    } else {
        // CONFIGURE TIMER COMMAND
        state[command.standard.channel].accessMode = command.standard.accessMode;
        state[command.standard.channel].operatingMode = command.standard.operatingMode;
        state[command.standard.channel].numberFormat = command.standard.numberFormat;

        // setup timer to be reset (timer is paused until load, waits for one virtual timer clock)
        state[command.standard.channel].waitingForLoad = true;
        switch (command.standard.accessMode) {
            // reset the access byte state machine
            case AccessMode::LowByteHighByte:
            case AccessMode::LowByteOnly:
                state[command.standard.channel].accessByte = ByteSelect::LowByte;
                state[command.standard.channel].writeByte = ByteSelect::LowByte;
                break;
            case AccessMode::HighByteOnly:
                state[command.standard.channel].accessByte = ByteSelect::HighByte;
                state[command.standard.channel].writeByte = ByteSelect::HighByte;
                break;
        }
    }
}

void ProgrammableIntervalTimer::writeRegister(uint8_t timer, uint8_t value) {
    assert(timer < 3);
    auto& selectedState = state[timer];

    // setup the reload value
    selectedState.pendingLoad = true;
    uint16_t currentReload = selectedState.reload;
    switch (selectedState.writeByte) {
        case ByteSelect::LowByte:
            if (selectedState.numberFormat == NumberFormat::Binary) {
                selectedState.reload = (currentReload & 0xFF00) | (uint16_t) value;
            } else {
                selectedState.reload = (currentReload - (currentReload % 100)) + value;
            }
            if (selectedState.accessMode == AccessMode::LowByteHighByte) {
                // there's another byte in set reload operation
                selectedState.writeByte = ByteSelect::HighByte;
            } else {
                // final byte in set reload operation
                if (selectedState.waitingForLoad) {
                    selectedState.waitingForLoad = false;
                    selectedState.pendingLoad = false;
                    selectedState.value = selectedState.reload;
                    selectedState.lastRecord = std::chrono::high_resolution_clock::now();
                }
            }
            break;
        case ByteSelect::HighByte:
            if (selectedState.numberFormat == NumberFormat::Binary) {
                selectedState.reload = (currentReload & 0x00FF) | ((uint16_t) value << 8);
            } else {
                selectedState.reload = (100 * (uint16_t) value) + (currentReload % 100);
            }
            if (selectedState.accessMode == AccessMode::LowByteHighByte) {
                // reset to low byte if we use a two byte reload operation
                selectedState.writeByte = ByteSelect::LowByte;
            }
            // always the final byte in set reload operation
            if (selectedState.waitingForLoad) {
                selectedState.waitingForLoad = false;
                    selectedState.pendingLoad = false;
                selectedState.value = selectedState.reload;
                selectedState.lastRecord = std::chrono::high_resolution_clock::now();

            }
            break;
    }
}

uint8_t ProgrammableIntervalTimer::readRegister(uint8_t timer) {
    assert(timer < 3);
    auto& selectedState = state[timer];
    uint16_t value = selectedState.latched ? selectedState.latch : selectedState.value;
    ReadBackCommandResult ret{};

    // resolve the result
    resolveTimers();
    switch (selectedState.accessByte) {
        case ByteSelect::LowByte:
            if (selectedState.numberFormat == NumberFormat::Binary) {
                ret.value = value & 0x00ff;
            } else {
                // BCD format
                ret.value = value % 100;
            }
            break;
        case ByteSelect::HighByte:
            if (selectedState.numberFormat == NumberFormat::Binary) {
                ret.value = (value >> 8) & 0x00ff;
            } else {
                // BCD format
                ret.value = value / 100;
            }
            break;
        case ByteSelect::StatusByte:
            ret.numberFormat = selectedState.numberFormat;
            ret.operatingMode = selectedState.operatingMode;
            ret.accessMode = selectedState.accessMode;
            ret.pendingLoad = selectedState.pendingLoad || selectedState.waitingForLoad;
            ret.outputState = selectedState.outputState;
            break;
    }

    // figure out the next byte to access
    switch (selectedState.accessByte) {
        case ByteSelect::LowByte:
            if (selectedState.accessMode == AccessMode::LowByteOnly) {
                selectedState.latched = false;
            } else {
                selectedState.accessByte = ByteSelect::HighByte;
            }
            break;
        case ByteSelect::HighByte:
            // high byte is always the last in the read state machine
            selectedState.latched = false;
            if (selectedState.accessMode != AccessMode::HighByteOnly) {
                selectedState.accessByte = ByteSelect::LowByte;
            }
            break;
        case ByteSelect::StatusByte:
            if (selectedState.accessMode == AccessMode::HighByteOnly) {
                // high byte only is the only ""
                selectedState.accessByte = ByteSelect::HighByte;
            } else {
                selectedState.accessByte = ByteSelect::LowByte;
            }
            break;
    }

    return ret.value;
}

// resolve the state of the timers
void ProgrammableIntervalTimer::resolveTimers() {
    auto now = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 3; i++) {
        // no operation if timer is paused
        if (state[i].waitingForLoad)
            continue;

        // get elapsed ticks
        auto elapsed = now - state[i].lastRecord;
        state[i].lastRecord = now;
        uint64_t ticks = elapsed.count() / (SourceClockPeriod * timerPrescaler);

        // did timer overflow?
        if (ticks > state[i].value) {
            ticks -= state[i].value;
            ticks %= (state[i].reload + 1);
            state[i].value = state[i].reload - ticks;
            state[i].pendingLoad = false;
        } else {
            state[i].value -= ticks;
        }

        // output value
        printf("timer %d value: %u\n", i, state[i].value);
    }
}

// DevicePio 8 bit interface
void ProgrammableIntervalTimer::iowrite8(uint16_t address, uint8_t data) {
    if (address & 0x3 == 0x03) {
        writeCommand({ .value = data });
    } else {
        writeRegister(address & 0x3, data);
    }
}

uint8_t ProgrammableIntervalTimer::ioread8(uint16_t address) {
    if (address & 0x3 != 0x03) {
        return readRegister(address & 0x3);
    }
    return 0;
}