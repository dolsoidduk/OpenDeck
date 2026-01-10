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

#pragma once

#include "application/io/common/common.h"

namespace io::buttons
{
    constexpr inline uint32_t DEBOUNCE_TIME_MS = 8;

    class Collection : public io::common::BaseCollection<PROJECT_TARGET_SUPPORTED_NR_OF_BUTTONS,
                                                         PROJECT_TARGET_SUPPORTED_NR_OF_ANALOG_INPUTS,
                                                         PROJECT_TARGET_SUPPORTED_NR_OF_TOUCHSCREEN_COMPONENTS>
    {
        public:
        Collection() = delete;
    };

    enum
    {
        GROUP_DIGITAL_INPUTS,
        GROUP_ANALOG_INPUTS,
        GROUP_TOUCHSCREEN_COMPONENTS
    };

    enum class type_t : uint8_t
    {
        MOMENTARY,    ///< Event on press and release.
        LATCHING,     ///< Event between presses only.
        AMOUNT        ///< Total number of button types.
    };

    /// Button message types
    /// @note See MIDI_BANK_CHANGE_GUIDE.md for detailed usage examples
    enum class messageType_t : uint8_t
    {
        NOTE,                              ///< Note On/Off message
        PROGRAM_CHANGE,                    ///< Direct Program Change (0-127)
        CONTROL_CHANGE,                    ///< Control Change message
        CONTROL_CHANGE_RESET,              ///< CC with reset on release
        MMC_STOP,                          ///< MIDI Machine Control Stop
        MMC_PLAY,                          ///< MIDI Machine Control Play
        MMC_RECORD,                        ///< MIDI Machine Control Record
        MMC_PAUSE,                         ///< MIDI Machine Control Pause
        REAL_TIME_CLOCK,                   ///< Real-time Clock message
        REAL_TIME_START,                   ///< Real-time Start message
        REAL_TIME_CONTINUE,                ///< Real-time Continue message
        REAL_TIME_STOP,                    ///< Real-time Stop message
        REAL_TIME_ACTIVE_SENSING,          ///< Real-time Active Sensing
        REAL_TIME_SYSTEM_RESET,            ///< Real-time System Reset
        PROGRAM_CHANGE_INC,                ///< Increment Program Change by 1
        PROGRAM_CHANGE_DEC,                ///< Decrement Program Change by 1
        NONE,                              ///< No message
        PRESET_CHANGE,                     ///< OpenDeck preset change
        MULTI_VAL_INC_RESET_NOTE,          ///< Multi-value increment/reset Note
        MULTI_VAL_INC_DEC_NOTE,            ///< Multi-value increment/decrement Note
        MULTI_VAL_INC_RESET_CC,            ///< Multi-value increment/reset CC
        MULTI_VAL_INC_DEC_CC,              ///< Multi-value increment/decrement CC
        NOTE_OFF_ONLY,                     ///< Send Note Off only
        CONTROL_CHANGE0_ONLY,              ///< Send CC with value 0 only
        BANK_SELECT_PROGRAM_CHANGE,        ///< Bank Select (MSB/LSB) + Program Change
                                           ///< Sends 3 MIDI messages:
                                           ///< 1. CC#0  (Bank MSB from VALUE[13:7])
                                           ///< 2. CC#32 (Bank LSB from VALUE[6:0])
                                           ///< 3. Program Change (from MIDI_ID)
                                           ///< Supports 16,384 banks × 128 programs
        PROGRAM_CHANGE_OFFSET_INC,         ///< Increment Program Change Offset
        PROGRAM_CHANGE_OFFSET_DEC,         ///< Decrement Program Change Offset
        BPM_INC,                           ///< Increment BPM
        BPM_DEC,                           ///< Decrement BPM
        MMC_PLAY_STOP,                     ///< MMC Play/Stop toggle
        NOTE_LEGATO,                       ///< Legato note (no Note Off)
        AMOUNT                             ///< Total number of message types
    };
}    // namespace io::buttons