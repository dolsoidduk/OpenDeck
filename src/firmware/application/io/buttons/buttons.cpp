/*

Copyright Igor Petrovic

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#ifdef PROJECT_TARGET_SUPPORT_BUTTONS

#include "buttons.h"
#include "application/system/config.h"
#include "application/global/midi_program.h"
#include "application/global/bpm.h"
#include "application/util/conversion/conversion.h"
#include "application/util/configurable/configurable.h"
#include "application/util/logger/logger.h"

#include "core/mcu.h"
#include "core/util/util.h"

#ifdef PROJECT_TARGET_SAX_REGISTER_CHROMATIC
#include "application/protocol/midi/common.h"
#endif

using namespace io::buttons;
using namespace protocol;

Buttons::Buttons(Hwa&      hwa,
                 Filter&   filter,
                 Database& database)
    : _hwa(hwa)
    , _filter(filter)
    , _database(database)
{
    MidiDispatcher.listen(messaging::eventType_t::ANALOG_BUTTON,
                          [this](const messaging::Event& event)
                          {
                              size_t     index = event.componentIndex + Collection::START_INDEX(GROUP_ANALOG_INPUTS);
                              Descriptor descriptor;
                              fillDescriptor(index, descriptor);

                              if (!event.forcedRefresh)
                              {
                                  // event.value in this case contains state information only
                                  processButton(index, event.value, descriptor);
                              }
                              else
                              {
                                  if (descriptor.type == type_t::LATCHING)
                                  {
                                      sendMessage(index, latchingState(index), descriptor);
                                  }
                                  else
                                  {
                                      sendMessage(index, state(index), descriptor);
                                  }
                              }
                          });

    MidiDispatcher.listen(messaging::eventType_t::TOUCHSCREEN_BUTTON,
                          [this](const messaging::Event& event)
                          {
                              size_t index = event.componentIndex + Collection::START_INDEX(GROUP_TOUCHSCREEN_COMPONENTS);

                              Descriptor descriptor;
                              fillDescriptor(index, descriptor);

                              // event.value in this case contains state information only
                              processButton(index, event.value, descriptor);
                          });

    MidiDispatcher.listen(messaging::eventType_t::SYSTEM,
                          [this](const messaging::Event& event)
                          {
                              switch (event.systemMessage)
                              {
                              case messaging::systemMessage_t::FORCE_IO_REFRESH:
                              {
                                  updateAll(true);
                              }
                              break;

                              default:
                                  break;
                              }
                          });

    ConfigHandler.registerConfig(
        sys::Config::block_t::BUTTONS,
        // read
        [this](uint8_t section, size_t index, uint16_t& value)
        {
            return sysConfigGet(static_cast<sys::Config::Section::button_t>(section), index, value);
        },

        // write
        [this](uint8_t section, size_t index, uint16_t value)
        {
            return sysConfigSet(static_cast<sys::Config::Section::button_t>(section), index, value);
        });
}

bool Buttons::init()
{
    for (size_t i = 0; i < Collection::SIZE(); i++)
    {
        reset(i);
    }

    return true;
}

void Buttons::updateSingle(size_t index, bool forceRefresh)
{
    if (index >= maxComponentUpdateIndex())
    {
        return;
    }

    Descriptor descriptor;

    uint8_t  numberOfReadings = 0;
    uint16_t states           = 0;

    if (!forceRefresh)
    {
        if (!state(index, numberOfReadings, states))
        {
            return;
        }

        fillDescriptor(index, descriptor);

        for (uint8_t reading = 0; reading < numberOfReadings; reading++)
        {
            // when processing, newest sample has index 0
            // start from oldest reading which is in upper bits
            uint8_t processIndex = numberOfReadings - 1 - reading;
            bool    state        = (states >> processIndex) & 0x01;

            if (!_filter.isFiltered(index, state))
            {
                continue;
            }

            processButton(index, state, descriptor);
        }
    }
    else
    {
        fillDescriptor(index, descriptor);

        if (descriptor.type == type_t::LATCHING)
        {
            sendMessage(index, latchingState(index), descriptor);
        }
        else
        {
            sendMessage(index, state(index), descriptor);
        }
    }
}

void Buttons::updateAll(bool forceRefresh)
{
    for (size_t i = 0; i < Collection::SIZE(GROUP_DIGITAL_INPUTS); i++)
    {
        updateSingle(i, forceRefresh);
    }
}

size_t Buttons::maxComponentUpdateIndex()
{
    return Collection::SIZE(GROUP_DIGITAL_INPUTS);
}

/// Handles changes in button states.
/// param [in]: index       Button index which has changed state.
/// param [in]: descriptor  Descriptor containing the entire configuration for the button.
void Buttons::processButton(size_t index, bool reading, Descriptor& descriptor)
{
    // act on change of state only
    if (reading == state(index))
    {
        return;
    }

    setState(index, reading);

#ifdef PROJECT_TARGET_SAX_REGISTER_CHROMATIC
    // Optional sax register-key chromatic mode.
    // When enabled, digital button events are combined into a single monophonic note stream.
    if (index < Collection::SIZE(GROUP_DIGITAL_INPUTS))
    {
        if (_database.read(database::Config::Section::system_t::SYSTEM_SETTINGS,
                           sys::Config::systemSetting_t::SAX_REGISTER_CHROMATIC_ENABLE))
        {
            processSaxRegisterChromatic();
            return;
        }
    }
#endif

    // don't process messageType_t::NONE type of message
    if (descriptor.messageType != messageType_t::NONE)
    {
        bool send = true;

        // NOTE_LEGATO always acts as MOMENTARY (process both press and release)
        if (descriptor.type == type_t::LATCHING && descriptor.messageType != messageType_t::NOTE_LEGATO)
        {
            // act on press only
            if (reading)
            {
                if (latchingState(index))
                {
                    setLatchingState(index, false);
                    // overwrite before processing
                    reading = false;
                }
                else
                {
                    setLatchingState(index, true);
                    reading = true;
                }
            }
            else
            {
                send = false;
            }
        }

        if (send)
        {
            sendMessage(index, reading, descriptor);
        }
    }
}

#ifdef PROJECT_TARGET_SAX_REGISTER_CHROMATIC
uint8_t Buttons::saxChannel() const
{
    // Use global channel (1-16). If set to OMNI or invalid, fall back to channel 1.
    const auto raw = static_cast<uint8_t>(
        _database.read(database::Config::Section::global_t::MIDI_SETTINGS,
                       protocol::midi::setting_t::GLOBAL_CHANNEL));

    if (raw >= 1 && raw <= 16)
    {
        return raw - 1;
    }

    return 0;
}

void Buttons::processSaxRegisterChromatic()
{
    const auto base = static_cast<uint8_t>(
        _database.read(database::Config::Section::system_t::SYSTEM_SETTINGS,
                       sys::Config::systemSetting_t::SAX_REGISTER_CHROMATIC_BASE_NOTE));

    // 0..48 where 24 == 0 semitones
    const uint16_t transposeRaw = static_cast<uint16_t>(
        _database.read(database::Config::Section::system_t::SYSTEM_SETTINGS,
                       sys::Config::systemSetting_t::SAX_REGISTER_CHROMATIC_TRANSPOSE));
    const int16_t transpose = static_cast<int16_t>(transposeRaw) - 24;

    const bool invertInputs = _database.read(
        database::Config::Section::system_t::SYSTEM_SETTINGS,
        sys::Config::systemSetting_t::SAX_REGISTER_CHROMATIC_INPUT_INVERT);

    int32_t    activeKey    = -1;
    const auto digitalCount = Collection::SIZE(GROUP_DIGITAL_INPUTS);
    const auto saxKeyCount  = (digitalCount < database::Config::SAX_FINGERING_KEYS) ? digitalCount : database::Config::SAX_FINGERING_KEYS;

    const auto saxPressed = [this, invertInputs](size_t index)
    {
        const bool pressed = state(index);
        return invertInputs ? !pressed : pressed;
    };

    // Priority: highest pressed index (deterministic).
    for (int32_t i = static_cast<int32_t>(digitalCount) - 1; i >= 0; i--)
    {
        if (saxPressed(static_cast<size_t>(i)))
        {
            activeKey = i;
            break;
        }
    }

    const uint8_t channel = saxChannel();

    // Build current fingering mask from first N digital keys.
    uint32_t currentMask = 0;
    for (size_t i = 0; i < saxKeyCount; i++)
    {
        if (saxPressed(i))
        {
            currentMask |= (1UL << i);
        }
    }

    const auto popcount32 = [](uint32_t v)
    {
        uint8_t c = 0;
        while (v)
        {
            c += static_cast<uint8_t>(v & 1U);
            v >>= 1U;
        }
        return c;
    };

    // Fingering table mode if any entry is enabled.
    bool     anyEnabled = false;
    bool     hasMatch   = false;
    uint8_t  bestScore  = 0;
    uint8_t  bestNote   = 0;

    for (size_t entry = 0; entry < database::Config::SAX_FINGERING_TABLE_ENTRIES; entry++)
    {
        const uint16_t hiEn = static_cast<uint16_t>(
            _database.read(database::Config::Section::global_t::SAX_FINGERING_MASK_HI10_ENABLE, entry));

        constexpr uint8_t HI_BITS = static_cast<uint8_t>(database::Config::SAX_FINGERING_KEYS - 14);
        constexpr uint16_t HI_MASK = static_cast<uint16_t>((1U << HI_BITS) - 1U);
        constexpr uint16_t ENABLE_MASK = static_cast<uint16_t>(1U << HI_BITS);

        const bool enabled = (hiEn & ENABLE_MASK) != 0;
        if (!enabled)
        {
            continue;
        }

        anyEnabled = true;

        const uint16_t lo14 = static_cast<uint16_t>(
            _database.read(database::Config::Section::global_t::SAX_FINGERING_MASK_LO14, entry));

        const uint16_t hi = static_cast<uint16_t>(hiEn & HI_MASK);
        uint32_t       mask = static_cast<uint32_t>(lo14) | (static_cast<uint32_t>(hi) << 14);

        // ignore bits outside of active key count
        if (saxKeyCount < database::Config::SAX_FINGERING_KEYS)
        {
            const uint32_t allowedMask = (saxKeyCount == 0U) ? 0U : ((1UL << saxKeyCount) - 1UL);
            mask &= allowedMask;
        }
        else
        {
            mask &= (database::Config::SAX_FINGERING_KEYS >= 32U)
                        ? 0xFFFFFFFFUL
                        : ((1UL << database::Config::SAX_FINGERING_KEYS) - 1UL);
        }

        if ((mask & currentMask) != mask)
        {
            continue;
        }

        const uint8_t score = popcount32(mask);
        if (!hasMatch || score > bestScore)
        {
            const uint16_t noteWide = static_cast<uint16_t>(
                _database.read(database::Config::Section::global_t::SAX_FINGERING_NOTE, entry));
            if (noteWide > 127)
            {
                continue;
            }

            bestScore = score;
            bestNote  = static_cast<uint8_t>(noteWide);
            hasMatch  = true;
        }
    }

    if (anyEnabled)
    {
        // If table is enabled, note is driven purely by the matched fingering.
        if (!hasMatch || currentMask == 0)
        {
            if (_saxNoteOn)
            {
                messaging::Event event = {};
                event.componentIndex   = 0;
                event.channel          = channel;
                event.index            = _saxActiveNote;
                event.value            = 0;
                event.message          = midi::messageType_t::NOTE_OFF;

                MidiDispatcher.notify(messaging::eventType_t::BUTTON, event);
                _saxNoteOn = false;
            }

            return;
        }

        int16_t noteWide = static_cast<int16_t>(bestNote) + transpose;
        if (noteWide < 0)
        {
            noteWide = 0;
        }
        else if (noteWide > 127)
        {
            noteWide = 127;
        }

        const uint8_t newNote = static_cast<uint8_t>(noteWide);

        if (_saxNoteOn && _saxActiveNote == newNote)
        {
            return;
        }

        if (_saxNoteOn)
        {
            messaging::Event offEvent = {};
            offEvent.componentIndex   = 0;
            offEvent.channel          = channel;
            offEvent.index            = _saxActiveNote;
            offEvent.value            = 0;
            offEvent.message          = midi::messageType_t::NOTE_OFF;
            MidiDispatcher.notify(messaging::eventType_t::BUTTON, offEvent);
        }

        messaging::Event onEvent = {};
        onEvent.componentIndex   = 0;
        onEvent.channel          = channel;
        onEvent.index            = newNote;
        onEvent.value            = 127;
        onEvent.message          = midi::messageType_t::NOTE_ON;
        MidiDispatcher.notify(messaging::eventType_t::BUTTON, onEvent);

        _saxActiveNote = newNote;
        _saxNoteOn     = true;
        return;
    }

    // Legacy mode (no fingering table entries enabled): keep existing behavior.
    if (activeKey < 0)
    {
        if (_saxNoteOn)
        {
            messaging::Event event = {};
            event.componentIndex   = 0;
            event.channel          = channel;
            event.index            = _saxActiveNote;
            event.value            = 0;
            event.message          = midi::messageType_t::NOTE_OFF;

            MidiDispatcher.notify(messaging::eventType_t::BUTTON, event);
            _saxNoteOn = false;
        }

        return;
    }

    const uint8_t mapRaw = static_cast<uint8_t>(
        _database.read(database::Config::Section::button_t::SAX_REGISTER_KEY_MAP,
                       static_cast<size_t>(activeKey)));

    uint16_t mappedKey = (mapRaw == 0)
                             ? static_cast<uint16_t>(activeKey)
                             : static_cast<uint16_t>(mapRaw - 1);

    // If mapping points outside of available digital inputs, fall back to identity.
    if (mappedKey >= digitalCount)
    {
        mappedKey = static_cast<uint16_t>(activeKey);
    }

    int16_t noteWide = static_cast<int16_t>(base) + static_cast<int16_t>(mappedKey) + transpose;
    if (noteWide < 0)
    {
        noteWide = 0;
    }
    else if (noteWide > 127)
    {
        noteWide = 127;
    }

    const uint8_t newNote = static_cast<uint8_t>(noteWide);

    if (_saxNoteOn && _saxActiveNote == newNote)
    {
        return;
    }

    if (_saxNoteOn)
    {
        messaging::Event offEvent = {};
        offEvent.componentIndex   = 0;
        offEvent.channel          = channel;
        offEvent.index            = _saxActiveNote;
        offEvent.value            = 0;
        offEvent.message          = midi::messageType_t::NOTE_OFF;
        MidiDispatcher.notify(messaging::eventType_t::BUTTON, offEvent);
    }

    messaging::Event onEvent = {};
    onEvent.componentIndex   = 0;
    onEvent.channel          = channel;
    onEvent.index            = newNote;
    onEvent.value            = 127;
    onEvent.message          = midi::messageType_t::NOTE_ON;
    MidiDispatcher.notify(messaging::eventType_t::BUTTON, onEvent);

    _saxActiveNote = newNote;
    _saxNoteOn     = true;
}

bool Buttons::captureSaxFingeringTableEntry(size_t entryIndex, uint16_t noteValue)
{
    if (entryIndex >= database::Config::SAX_FINGERING_TABLE_ENTRIES)
    {
        return false;
    }

    const auto digitalCount = Collection::SIZE(GROUP_DIGITAL_INPUTS);
    const auto saxKeyCount  = (digitalCount < database::Config::SAX_FINGERING_KEYS) ? digitalCount : database::Config::SAX_FINGERING_KEYS;

    const bool invertInputs = _database.read(
        database::Config::Section::system_t::SYSTEM_SETTINGS,
        sys::Config::systemSetting_t::SAX_REGISTER_CHROMATIC_INPUT_INVERT);

    const auto saxPressed = [this, invertInputs](size_t index)
    {
        const bool pressed = state(index);
        return invertInputs ? !pressed : pressed;
    };

    uint32_t currentMask = 0;
    for (size_t i = 0; i < saxKeyCount; i++)
    {
        if (saxPressed(i))
        {
            currentMask |= (1UL << i);
        }
    }

    constexpr uint8_t  HI_BITS     = static_cast<uint8_t>(database::Config::SAX_FINGERING_KEYS - 14);
    constexpr uint16_t HI_MASK     = static_cast<uint16_t>((1U << HI_BITS) - 1U);
    constexpr uint16_t ENABLE_MASK = static_cast<uint16_t>(1U << HI_BITS);

    const uint16_t lo14 = static_cast<uint16_t>(currentMask & 0x3FFFU);
    const uint16_t hi   = static_cast<uint16_t>((currentMask >> 14U) & HI_MASK);
    const uint16_t hiEn = static_cast<uint16_t>(hi | ENABLE_MASK);

    bool ok = true;
    ok &= _database.update(database::Config::Section::global_t::SAX_FINGERING_MASK_LO14, entryIndex, lo14);
    ok &= _database.update(database::Config::Section::global_t::SAX_FINGERING_MASK_HI10_ENABLE, entryIndex, hiEn);

    if (noteValue <= 127)
    {
        ok &= _database.update(database::Config::Section::global_t::SAX_FINGERING_NOTE, entryIndex, noteValue);
    }

    return ok;
}
#endif

/// Used to send MIDI message from specified button.
/// Used internally once the button state has been changed and processed.
/// param [in]: index           Button index which sends the message.
/// param [in]: descriptor      Structure holding all the information about button for specified index.
void Buttons::sendMessage(size_t index, bool state, Descriptor& descriptor)
{
    bool send      = true;
    auto eventType = messaging::eventType_t::BUTTON;

    if (state)
    {
        switch (descriptor.messageType)
        {
        case messageType_t::NOTE:
        case messageType_t::CONTROL_CHANGE:
        case messageType_t::CONTROL_CHANGE_RESET:
        case messageType_t::REAL_TIME_CLOCK:
        case messageType_t::REAL_TIME_START:
        case messageType_t::REAL_TIME_CONTINUE:
        case messageType_t::REAL_TIME_STOP:
        case messageType_t::REAL_TIME_ACTIVE_SENSING:
        case messageType_t::REAL_TIME_SYSTEM_RESET:
        case messageType_t::MMC_PLAY:
        case messageType_t::MMC_STOP:
        case messageType_t::MMC_PAUSE:
        case messageType_t::MMC_RECORD:
        case messageType_t::MMC_PLAY_STOP:
            break;

        case messageType_t::NOTE_LEGATO:
        {
            // Monophonic legato: last-note priority
            // On press: send Note On for the new note, and send Note Off for the previous active note
            // so only one note is active per channel.

            uint8_t channel = descriptor.event.channel & 0x0F;
            uint8_t newNote = descriptor.event.index & 0x7F;

            // Increment pressed count for this channel
            _legatoButtonCount[channel]++;

            // If there was an active note on this channel and it's different, turn it off
            if (_legatoButtonCount[channel] > 1)
            {
                uint8_t prevNote = _legatoActiveNote[channel];
                if (prevNote != newNote)
                {
                    messaging::Event offEvent = descriptor.event;
                    offEvent.index            = prevNote;
                    offEvent.value            = 0;
                    offEvent.message          = midi::messageType_t::NOTE_OFF;

                    MidiDispatcher.notify(eventType, offEvent);
                }
            }

            // Update active note to the newly pressed one and ensure Note On
            _legatoActiveNote[channel] = newNote;
            descriptor.event.message   = midi::messageType_t::NOTE_ON;
        }
        break;

        case messageType_t::PROGRAM_CHANGE:
        {
            descriptor.event.value = 0;
            descriptor.event.index += MidiProgram.offset();
            descriptor.event.index &= 0x7F;
        }
        break;

        case messageType_t::PROGRAM_CHANGE_INC:
        {
            descriptor.event.value = 0;

            if (!MidiProgram.incrementProgram(descriptor.event.channel, 1))
            {
                send = false;
            }

            descriptor.event.index = MidiProgram.program(descriptor.event.channel);
        }
        break;

        case messageType_t::PROGRAM_CHANGE_DEC:
        {
            descriptor.event.value = 0;

            if (!MidiProgram.decrementProgram(descriptor.event.channel, 1))
            {
                send = false;
            }

            descriptor.event.index = MidiProgram.program(descriptor.event.channel);
        }
        break;

        case messageType_t::MULTI_VAL_INC_RESET_NOTE:
        {
            auto newValue = ValueIncDecMIDI7Bit::increment(_incDecValue[index],
                                                           descriptor.event.value,
                                                           ValueIncDecMIDI7Bit::type_t::OVERFLOW);

            if (newValue != _incDecValue[index])
            {
                if (!newValue)
                {
                    descriptor.event.message = midi::messageType_t::NOTE_OFF;
                }
                else
                {
                    descriptor.event.message = midi::messageType_t::NOTE_ON;
                }

                _incDecValue[index]    = newValue;
                descriptor.event.value = newValue;
            }
            else
            {
                send = false;
            }
        }
        break;

        case messageType_t::MULTI_VAL_INC_DEC_NOTE:
        {
            auto newValue = ValueIncDecMIDI7Bit::increment(_incDecValue[index],
                                                           descriptor.event.value,
                                                           ValueIncDecMIDI7Bit::type_t::EDGE);

            if (newValue != _incDecValue[index])
            {
                if (!newValue)
                {
                    descriptor.event.message = midi::messageType_t::NOTE_OFF;
                }
                else
                {
                    descriptor.event.message = midi::messageType_t::NOTE_ON;
                }

                _incDecValue[index]    = newValue;
                descriptor.event.value = newValue;
            }
            else
            {
                send = false;
            }
        }
        break;

        case messageType_t::MULTI_VAL_INC_RESET_CC:
        {
            auto newValue = ValueIncDecMIDI7Bit::increment(_incDecValue[index],
                                                           descriptor.event.value,
                                                           ValueIncDecMIDI7Bit::type_t::OVERFLOW);

            if (newValue != _incDecValue[index])
            {
                _incDecValue[index]    = newValue;
                descriptor.event.value = newValue;
            }
            else
            {
                send = false;
            }
        }
        break;

        case messageType_t::MULTI_VAL_INC_DEC_CC:
        {
            auto newValue = ValueIncDecMIDI7Bit::increment(_incDecValue[index],
                                                           descriptor.event.value,
                                                           ValueIncDecMIDI7Bit::type_t::EDGE);

            if (newValue != _incDecValue[index])
            {
                _incDecValue[index]    = newValue;
                descriptor.event.value = newValue;
            }
            else
            {
                send = false;
            }
        }
        break;

        case messageType_t::NOTE_OFF_ONLY:
        {
            descriptor.event.value   = 0;
            descriptor.event.message = midi::messageType_t::NOTE_OFF;
        }
        break;

        case messageType_t::CONTROL_CHANGE0_ONLY:
        {
            descriptor.event.value = 0;
        }
        break;

        case messageType_t::BANK_SELECT_PROGRAM_CHANGE:
        {
            // MIDI Bank Select + Program Change
            // -----------------------------------
            // Implements the standard MIDI Bank/Program Change protocol.
            // This message expands into 3 sequential MIDI messages:
            //
            // 1) CC#0  (Bank Select MSB) - Upper 7 bits of 14-bit bank number
            // 2) CC#32 (Bank Select LSB) - Lower 7 bits of 14-bit bank number
            // 3) Program Change          - Program number from MIDI_ID (0-127)
            //
            // Configuration:
            //   - MIDI_ID: Program Change number (0-127)
            //   - VALUE:   14-bit Bank number (0-16383)
            //              Bank MSB = VALUE >> 7
            //              Bank LSB = VALUE & 0x7F
            //   - Channel: MIDI channel (1-16)
            //
            // Example: Bank 3:7 (MSB=3, LSB=7) + Program 12
            //   VALUE = (3 << 7) | 7 = 391
            //   MIDI_ID = 12
            //   Result: CC#0=3, CC#32=7, PC=12
            //
            // Total addressable space: 16,384 banks × 128 programs = 2,097,152 presets
            // See MIDI_BANK_CHANGE_GUIDE.md for configuration examples.

            const uint16_t bank    = descriptor.event.value & 0x3FFF;    // 14-bit bank (0-16383)
            const uint8_t  bankMsb = (bank >> 7) & 0x7F;                 // Upper 7 bits
            const uint8_t  bankLsb = bank & 0x7F;                        // Lower 7 bits

            // Send CC#0 (Bank Select MSB)
            auto ccMsb    = descriptor.event;
            ccMsb.message = midi::messageType_t::CONTROL_CHANGE;
            ccMsb.index   = 0;
            ccMsb.value   = bankMsb;
            MidiDispatcher.notify(eventType, ccMsb);

            // Send CC#32 (Bank Select LSB)
            auto ccLsb    = descriptor.event;
            ccLsb.message = midi::messageType_t::CONTROL_CHANGE;
            ccLsb.index   = 32;
            ccLsb.value   = bankLsb;
            MidiDispatcher.notify(eventType, ccLsb);

            // Send Program Change
            auto pc    = descriptor.event;
            pc.message = midi::messageType_t::PROGRAM_CHANGE;
            pc.index &= 0x7F;
            pc.value = 0;
            MidiDispatcher.notify(eventType, pc);

            // We've already dispatched all MIDI events.
            send = false;
        }
        break;

        case messageType_t::CUSTOM_SYS_EX:
        {
            // SysExConf transports values as 7-bit safe bytes, and the config protocol supports 14-bit values.
            // Store only payload bytes between F0 and F7 (each must be 0..0x7F), packed 2 bytes -> 14-bit word:
            // packed = b0 | (b1 << 7)
            static constexpr uint8_t MAX_TOTAL_LEN    = 16; // includes F0 and F7
            static constexpr uint8_t MAX_PAYLOAD_LEN  = MAX_TOTAL_LEN - 2;
            static constexpr uint8_t CUSTOM_SYSEX_WORDS = MAX_TOTAL_LEN / 2; // keep 8 words in DB

            const uint8_t payloadLen = static_cast<uint8_t>(
                _database.read(database::Config::Section::button_t::SYSEX_LENGTH, index));

            if (payloadLen == 0 || payloadLen > MAX_PAYLOAD_LEN)
            {
                send = false;
                break;
            }

            uint8_t payloadBuf[MAX_PAYLOAD_LEN] = {};

            for (uint8_t wordIndex = 0; wordIndex < CUSTOM_SYSEX_WORDS; wordIndex++)
            {
                const auto section = static_cast<database::Config::Section::button_t>(
                    static_cast<uint8_t>(database::Config::Section::button_t::SYSEX_DATA_0) + wordIndex);

                const uint16_t word = static_cast<uint16_t>(_database.read(section, index));

                const uint8_t b0 = static_cast<uint8_t>(word & 0x7F);
                const uint8_t b1 = static_cast<uint8_t>((word >> 7) & 0x7F);

                const uint8_t outIndex0 = static_cast<uint8_t>(2 * wordIndex);
                const uint8_t outIndex1 = static_cast<uint8_t>(2 * wordIndex + 1);

                if (outIndex0 < MAX_PAYLOAD_LEN)
                {
                    payloadBuf[outIndex0] = b0;
                }
                if (outIndex1 < MAX_PAYLOAD_LEN)
                {
                    payloadBuf[outIndex1] = b1;
                }
            }

            uint8_t sysExBuf[MAX_TOTAL_LEN] = {};
            const uint8_t length = static_cast<uint8_t>(payloadLen + 2);

            sysExBuf[0] = 0xF0;
            for (uint8_t i = 0; i < payloadLen; i++)
            {
                sysExBuf[1 + i] = payloadBuf[i];
            }
            sysExBuf[1 + payloadLen] = 0xF7;

            const uint8_t varPos   = static_cast<uint8_t>(descriptor.event.index & 0xFF);
            const uint8_t varValue = static_cast<uint8_t>(descriptor.event.value & 0x7F);

            // Variable substitution is optional.
            // Treat index 0 as 'disabled' so default config doesn't corrupt the leading 0xF0.
            // Note: varPos uses full-message indexing (including F0 at index 0).
            if (varPos != 0 && varPos < (length - 1))
            {
                sysExBuf[varPos] = varValue;
            }

#ifdef OPENDECK_DEBUG_SYSEX_TRACE
            // Debug trace (visible in any MIDI monitor).
            // Sends a non-commercial SysEx (0x7D) that encodes the first bytes as nibbles (7-bit safe).
            // Format:
            // F0 7D 'O' 'D' 01 <btnIdx> <len> <varPos> <varVal> <hi/lo nibbles...> F7
            {
                uint8_t traceBuf[64] = {};
                uint8_t traceLen     = 0;

                traceBuf[traceLen++] = 0xF0;
                traceBuf[traceLen++] = 0x7D;
                traceBuf[traceLen++] = 0x4F;    // 'O'
                traceBuf[traceLen++] = 0x44;    // 'D'
                traceBuf[traceLen++] = 0x01;    // version
                traceBuf[traceLen++] = static_cast<uint8_t>(index & 0x7F);
                traceBuf[traceLen++] = static_cast<uint8_t>(length & 0x7F);
                traceBuf[traceLen++] = static_cast<uint8_t>(varPos & 0x7F);
                traceBuf[traceLen++] = static_cast<uint8_t>(varValue & 0x7F);

                const uint8_t traceBytes = (length < 8) ? length : 8;
                for (uint8_t i = 0; i < traceBytes; i++)
                {
                    traceBuf[traceLen++] = static_cast<uint8_t>((sysExBuf[i] >> 4) & 0x0F);
                    traceBuf[traceLen++] = static_cast<uint8_t>(sysExBuf[i] & 0x0F);
                }

                traceBuf[traceLen++] = 0xF7;

                auto traceEvent       = descriptor.event;
                traceEvent.sysEx      = traceBuf;
                traceEvent.sysExLength = traceLen;
                traceEvent.message    = midi::messageType_t::SYS_EX;

                MidiDispatcher.notify(eventType, traceEvent);

                LOG_INF("Custom SysEx trace: btn=%d len=%d varPos=%d varVal=%d",
                        static_cast<int>(index),
                        static_cast<int>(length),
                        static_cast<int>(varPos),
                        static_cast<int>(varValue));
            }
#endif

            descriptor.event.sysEx       = sysExBuf;
            descriptor.event.sysExLength = length;
            descriptor.event.message     = midi::messageType_t::SYS_EX;
        }
        break;

        case messageType_t::PROGRAM_CHANGE_OFFSET_INC:
        {
            MidiProgram.incrementOffset(descriptor.event.value);
        }
        break;

        case messageType_t::PROGRAM_CHANGE_OFFSET_DEC:
        {
            MidiProgram.decrementOffset(descriptor.event.value);
        }
        break;

        case messageType_t::PRESET_CHANGE:
        {
            eventType                      = messaging::eventType_t::SYSTEM;
            descriptor.event.systemMessage = messaging::systemMessage_t::PRESET_CHANGE_DIRECT_REQ;
        }
        break;

        case messageType_t::BPM_INC:
        {
            descriptor.event.value = 0;

            if (!Bpm.increment(1))
            {
                send = false;
            }

            descriptor.event.index = Bpm.value();
        }
        break;

        case messageType_t::BPM_DEC:
        {
            descriptor.event.value = 0;

            if (!Bpm.decrement(1))
            {
                send = false;
            }

            descriptor.event.index = Bpm.value();
        }
        break;

        default:
        {
            send = false;
        }
        break;
        }
    }
    else
    {
        switch (descriptor.messageType)
        {
        case messageType_t::NOTE:
        {
            descriptor.event.value   = 0;
            descriptor.event.message = midi::messageType_t::NOTE_OFF;
        }
        break;

        case messageType_t::NOTE_LEGATO:
        {
            // Monophonic legato: final release turns off the current active note only
            uint8_t channel = descriptor.event.channel & 0x0F;

            if (_legatoButtonCount[channel] > 0)
            {
                _legatoButtonCount[channel]--;
            }

            if (_legatoButtonCount[channel] == 0)
            {
                // No buttons held anymore on this channel - turn off the active note
                descriptor.event.index   = _legatoActiveNote[channel];
                descriptor.event.value   = 0;
                descriptor.event.message = midi::messageType_t::NOTE_OFF;

                // Clear active note marker (optional)
                _legatoActiveNote[channel] = 0;
            }
            else
            {
                // Still other buttons pressed - suppress Note Off
                send = false;
            }
        }
        break;

        case messageType_t::CONTROL_CHANGE_RESET:
        {
            descriptor.event.value = 0;
        }
        break;

        case messageType_t::MMC_RECORD:
        {
            descriptor.event.message = midi::messageType_t::MMC_RECORD_STOP;
        }
        break;

        case messageType_t::MMC_PLAY_STOP:
        {
            descriptor.event.message = midi::messageType_t::MMC_STOP;
        }
        break;

        default:
        {
            send = false;
        }
        break;
        }
    }

    if (send)
    {
        MidiDispatcher.notify(eventType, descriptor.event);
    }
}

/// Updates current state of button.
/// param [in]: index       Button for which state is being changed.
/// param [in]: state       New button state (true/pressed, false/released).
void Buttons::setState(size_t index, bool state)
{
    uint8_t arrayIndex = index / 8;
    uint8_t bit        = index - 8 * arrayIndex;

    core::util::BIT_WRITE(_buttonPressed[arrayIndex], bit, state);
}

/// Checks for last button state.
/// param [in]: index    Button index for which previous state is being checked.
/// returns: True if last state was on/pressed, false otherwise.
bool Buttons::state(size_t index)
{
    uint8_t arrayIndex = index / 8;
    uint8_t bit        = index - 8 * arrayIndex;

    return core::util::BIT_READ(_buttonPressed[arrayIndex], bit);
}

/// Updates current state of latching button.
/// Used only for latching buttons where new state which should be sent differs
/// from last one, for instance when sending MIDI note on on first press (latching
/// state: true), and note off on second (latching state: false).
/// State should be stored in variable because unlike momentary buttons, state of
/// latching buttons doesn't necessarrily match current "real" state of button since events
/// for latching buttons are sent only on presses.
/// param [in]: index    Button for which state is being changed.
/// param [in]: state       New latching state.
void Buttons::setLatchingState(size_t index, bool state)
{
    uint8_t arrayIndex = index / 8;
    uint8_t bit        = index - 8 * arrayIndex;

    core::util::BIT_WRITE(_lastLatchingState[arrayIndex], bit, state);
}

/// Checks for last latching button state.
/// param [in]: index    Button index for which previous latching state is being checked.
/// returns: True if last state was on/pressed, false otherwise.
bool Buttons::latchingState(size_t index)
{
    uint8_t arrayIndex = index / 8;
    uint8_t bit        = index - 8 * arrayIndex;

    return core::util::BIT_READ(_lastLatchingState[arrayIndex], bit);
}

/// Resets the current state of the specified button.
/// param [in]: index    Button for which to reset state.
void Buttons::reset(size_t index)
{
    setState(index, false);
    setLatchingState(index, false);
}

void Buttons::fillDescriptor(size_t index, Descriptor& descriptor)
{
    descriptor.type                 = static_cast<type_t>(_database.read(database::Config::Section::button_t::TYPE, index));
    descriptor.messageType          = static_cast<messageType_t>(_database.read(database::Config::Section::button_t::MESSAGE_TYPE, index));
    descriptor.event.componentIndex = index;
    descriptor.event.channel        = _database.read(database::Config::Section::button_t::CHANNEL, index);
    descriptor.event.index          = _database.read(database::Config::Section::button_t::MIDI_ID, index);
    descriptor.event.value          = _database.read(database::Config::Section::button_t::VALUE, index);

    descriptor.event.message = INTERNAL_MSG_TO_MIDI_TYPE[static_cast<uint8_t>(descriptor.messageType)];
}

bool Buttons::state(size_t index, uint8_t& numberOfReadings, uint16_t& states)
{
    // if encoder under this index is enabled, just return false state each time
    if (_database.read(database::Config::Section::encoder_t::ENABLE, _hwa.buttonToEncoderIndex(index)))
    {
        return false;
    }

    return _hwa.state(index, numberOfReadings, states);
}

std::optional<uint8_t> Buttons::sysConfigGet(sys::Config::Section::button_t section, size_t index, uint16_t& value)
{
    uint32_t readValue;

    auto result = _database.read(util::Conversion::SYS_2_DB_SECTION(section), index, readValue)
                      ? sys::Config::Status::ACK
                      : sys::Config::Status::ERROR_READ;

    value = readValue;

    return result;
}

std::optional<uint8_t> Buttons::sysConfigSet(sys::Config::Section::button_t section, size_t index, uint16_t value)
{
    auto result = _database.update(util::Conversion::SYS_2_DB_SECTION(section), index, value)
                      ? sys::Config::Status::ACK
                      : sys::Config::Status::ERROR_WRITE;

    if (result == sys::Config::Status::ACK)
    {
        if (
            (section == sys::Config::Section::button_t::TYPE) ||
            (section == sys::Config::Section::button_t::MESSAGE_TYPE))
        {
            reset(index);
        }
    }

    return result;
}

#endif