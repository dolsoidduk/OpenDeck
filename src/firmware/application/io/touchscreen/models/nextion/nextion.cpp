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

#ifdef PROJECT_TARGET_SUPPORT_TOUCHSCREEN

#include "nextion.h"

#include "application/io/touchscreen/touchscreen.h"

#include "core/mcu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using namespace io::touchscreen;

Nextion::Nextion(Hwa& hwa)
    : _hwa(hwa)
{
    Touchscreen::registerModel(model_t::NEXTION, this);
}

bool Nextion::init()
{
    Model::_bufferCount = 0;

    if (_hwa.init())
    {
        // Avoid blocking delays during system boot: USB servicing happens in board::update()
        // which starts only after System::init() returns.
        _postInitPending   = true;
        _postInitReadyAtMs = core::mcu::timing::ms() + 1000;

        return true;
    }

    return false;
}

bool Nextion::deInit()
{
    _postInitPending        = false;
    _postInitReadyAtMs      = 0;
    _pendingScreenValid     = false;
    _pendingBrightnessValid = false;
    return _hwa.deInit();
}

bool Nextion::setScreen(size_t index)
{
    maybeFinishPostInit();

    if (_postInitPending)
    {
        _pendingScreenValid = true;
        _pendingScreenIndex = index;
        return true;
    }

    return writeCommand("page %u", index);
}

tsEvent_t Nextion::update(Data& data)
{
    maybeFinishPostInit();

    uint8_t value   = 0;
    bool    process = false;
    auto    retVal  = tsEvent_t::NONE;

    while (_hwa.read(value))
    {
        Model::_rxBuffer[Model::_bufferCount++] = value;

        if (value == 0xFF)
        {
            _endCounter++;
        }
        else
        {
            if (_endCounter)
            {
                _endCounter = 0;
            }
        }

        if (_endCounter == 3)
        {
            // new message arrived
            _endCounter = 0;
            process     = true;
            break;
        }
    }

    if (process)
    {
        retVal              = response(data);
        Model::_bufferCount = 0;
    }

    return retVal;
}

void Nextion::setIconState(Icon& icon, bool state)
{
    maybeFinishPostInit();

    if (_postInitPending)
    {
        return;
    }

    // ignore width/height zero - set either intentionally to avoid display or incorrectly
    if (!icon.width)
    {
        return;
    }

    if (!icon.height)
    {
        return;
    }

    writeCommand("picq %u,%u,%u,%u,%u", icon.xPos, icon.yPos, icon.width, icon.height, state ? icon.onScreen : icon.offScreen);
}

bool Nextion::writeCommand(const char* line, ...)
{
    va_list args;
    va_start(args, line);

    int retVal = vsnprintf(_commandBuffer, Model::BUFFER_SIZE, line, args);

    va_end(args);

    if (retVal < 0)
    {
        return false;
    }

    for (size_t i = 0; i < strlen(_commandBuffer); i++)
    {
        if (!_hwa.write(_commandBuffer[i]))
        {
            return false;
        }
    }

    return endCommand();
}

bool Nextion::endCommand()
{
    for (int i = 0; i < 3; i++)
    {
        if (!_hwa.write(0xFF))
        {
            return false;
        }
    }

    return true;
}

bool Nextion::setBrightness(brightness_t brightness)
{
    maybeFinishPostInit();

    if (_postInitPending)
    {
        _pendingBrightnessValid = true;
        _pendingBrightness      = brightness;
        return true;
    }

    return writeCommand("dims=%d", BRIGHTNESS_MAPPING[static_cast<uint8_t>(brightness)]);
}

void Nextion::maybeFinishPostInit()
{
    if (!_postInitPending)
    {
        return;
    }

    if (core::mcu::timing::ms() < _postInitReadyAtMs)
    {
        return;
    }

    // Mark as ready before sending any commands to avoid recursion via writeCommand().
    _postInitPending = false;

    endCommand();
    writeCommand("sendxy=1");

    if (_pendingScreenValid)
    {
        writeCommand("page %u", _pendingScreenIndex);
        _pendingScreenValid = false;
    }

    if (_pendingBrightnessValid)
    {
        writeCommand("dims=%d", BRIGHTNESS_MAPPING[static_cast<uint8_t>(_pendingBrightness)]);
        _pendingBrightnessValid = false;
    }
}

tsEvent_t Nextion::response(Data& data)
{
    bool responseFound = false;
    auto response      = responseId_t::BUTTON;    // assumption for now

    for (size_t i = 0; i < static_cast<size_t>(responseId_t::AMOUNT); i++)
    {
        if (Model::_bufferCount == RESPONSES[i].size)
        {
            if (Model::_rxBuffer[0] == static_cast<uint8_t>(RESPONSES[i].responseId))
            {
                response      = static_cast<responseId_t>(i);
                responseFound = true;
                break;
            }
        }
    }

    if (responseFound)
    {
        switch (response)
        {
        case responseId_t::BUTTON:
        {
            data.buttonState = Model::_rxBuffer[1];
            data.buttonIndex = Model::_rxBuffer[2];

            return tsEvent_t::BUTTON;
        }
        break;

        default:
            return tsEvent_t::NONE;
        }
    }

    return tsEvent_t::NONE;
}

#endif