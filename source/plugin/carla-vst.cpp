/*
 * Carla Native Plugins
 * Copyright (C) 2013-2022 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#ifdef __WINE__
  #error This file is not supposed to be built with wine!
#endif
#ifndef CARLA_PLUGIN_SYNTH
  #error CARLA_PLUGIN_SYNTH undefined
#endif

#ifndef CARLA_VST_SHELL
  #ifndef CARLA_PLUGIN_PATCHBAY
    #error CARLA_PLUGIN_PATCHBAY undefined
  #endif
  #if defined(CARLA_PLUGIN_64CH) || defined(CARLA_PLUGIN_32CH) || defined(CARLA_PLUGIN_16CH)
    #if ! CARLA_PLUGIN_SYNTH
      #error CARLA_PLUGIN_16/32/64CH requires CARLA_PLUGIN_SYNTH
    #endif
  #endif
#endif

#define CARLA_NATIVE_PLUGIN_VST
#include "carla-base.cpp"
#include "carla-vst.hpp"

#include "water/files/File.h"

#include "CarlaMathUtils.hpp"
#include "CarlaVst2Utils.hpp"

static uint32_t d_lastBufferSize = 0;
static double   d_lastSampleRate = 0.0;

static const int32_t kBaseUniqueID = CCONST('C', 'r', 'l', 'a');
static const int32_t kVstMidiEventSize = static_cast<int32_t>(sizeof(VstMidiEvent));

#ifdef CARLA_VST_SHELL
# if CARLA_PLUGIN_SYNTH
static const int32_t kShellUniqueID = CCONST('C', 'r', 'l', 's');
# else
static const int32_t kShellUniqueID = CCONST('C', 'r', 'l', 'F');
# endif
#else
static const int32_t kNumParameters = 100;
#endif

static const bool kIsUsingUILauncher = isUsingUILauncher();

// --------------------------------------------------------------------------------------------------------------------
// Carla Internal Plugin API exposed as VST plugin

class NativePlugin
{
public:
    static const uint32_t kMaxMidiEvents = 512;

    NativePlugin(AEffect* const effect, const NativePluginDescriptor* desc)
        : fEffect(effect),
          fHandle(nullptr),
          fHost(),
          fDescriptor(desc),
          fBufferSize(d_lastBufferSize),
          fSampleRate(d_lastSampleRate),
          fIsActive(false),
          fMidiEventCount(0),
          fTimeInfo(),
          fVstRect(),
          fUiLauncher(nullptr),
          fHostType(kHostTypeNull),
          fMidiOutEvents(),
          fStateChunk(nullptr)
    {
        fHost.handle      = this;
        fHost.uiName      = carla_strdup("CarlaVST");
        fHost.uiParentId  = 0;

        std::memset(fProgramName, 0, sizeof(fProgramName));
        std::strcpy(fProgramName, "Default");

        // find resource dir
        using water::File;
        using water::String;

        File curExe = File::getSpecialLocation(File::currentExecutableFile).getLinkedTarget();
        File resDir = curExe.getSiblingFile("resources");

#ifndef CARLA_OS_MAC
        // FIXME: proper fallback path for other OSes
        if (! resDir.exists())
            resDir = File("/usr/local/share/carla/resources");
        if (! resDir.exists())
            resDir = File("/usr/share/carla/resources");
#endif

        // find host type
        const String hostFilename(File::getSpecialLocation(File::hostApplicationPath).getFileName());

        /**/ if (hostFilename.startsWith("ardour"))
            fHostType = kHostTypeArdour;
        else if (hostFilename.startsWith("Bitwig"))
            fHostType = kHostTypeBitwig;

        fHost.resourceDir = carla_strdup(resDir.getFullPathName().toRawUTF8());

        fHost.get_buffer_size        = host_get_buffer_size;
        fHost.get_sample_rate        = host_get_sample_rate;
        fHost.is_offline             = host_is_offline;
        fHost.get_time_info          = host_get_time_info;
        fHost.write_midi_event       = host_write_midi_event;
        fHost.ui_parameter_changed   = host_ui_parameter_changed;
        fHost.ui_custom_data_changed = host_ui_custom_data_changed;
        fHost.ui_closed              = host_ui_closed;
        fHost.ui_open_file           = host_ui_open_file;
        fHost.ui_save_file           = host_ui_save_file;
        fHost.dispatcher             = host_dispatcher;

        fVstRect.top  = 0;
        fVstRect.left = 0;

        if (kIsUsingUILauncher || (fDescriptor->hints & NATIVE_PLUGIN_USES_UI_SIZE) == 0x0)
        {
            fVstRect.right  = ui_launcher_res::carla_uiWidth;
            fVstRect.bottom = ui_launcher_res::carla_uiHeight;
        }
        else
        {
            fVstRect.right  = static_cast<int16_t>(fDescriptor->ui_width);
            fVstRect.bottom = static_cast<int16_t>(fDescriptor->ui_height);
        }

        init();
    }

    ~NativePlugin()
    {
        if (fIsActive)
        {
            // host has not de-activated the plugin yet, nasty!
            fIsActive = false;

            if (fDescriptor->deactivate != nullptr)
                fDescriptor->deactivate(fHandle);
        }

        if (fDescriptor->cleanup != nullptr && fHandle != nullptr)
            fDescriptor->cleanup(fHandle);

        fHandle = nullptr;

        if (fStateChunk != nullptr)
        {
            std::free(fStateChunk);
            fStateChunk = nullptr;
        }

        if (fHost.uiName != nullptr)
        {
            delete[] fHost.uiName;
            fHost.uiName = nullptr;
        }

        if (fHost.resourceDir != nullptr)
        {
            delete[] fHost.resourceDir;
            fHost.resourceDir = nullptr;
        }
    }

    bool init()
    {
        if (fDescriptor->instantiate == nullptr || fDescriptor->process == nullptr)
        {
            carla_stderr("Plugin is missing something...");
            return false;
        }

        fHandle = fDescriptor->instantiate(&fHost);
        CARLA_SAFE_ASSERT_RETURN(fHandle != nullptr, false);

        carla_zeroStructs(fMidiEvents, kMaxMidiEvents);
        carla_zeroStruct(fTimeInfo);

        return true;
    }

    const NativePluginDescriptor* getDescriptor() const noexcept
    {
        return fDescriptor;
    }

    // -------------------------------------------------------------------

    intptr_t vst_dispatcher(const int32_t opcode,
                            const int32_t index, const intptr_t value, void* const ptr, const float opt)
    {
        CARLA_SAFE_ASSERT_RETURN(fHandle != nullptr, 0);

        intptr_t ret = 0;

        switch (opcode)
        {
        case effGetProgram:
            return 0;

        case effSetProgramName:
            if (char* const programName = (char*)ptr)
            {
                std::strncpy(fProgramName, programName, 32);
                return 1;
            }
            break;

        case effGetProgramName:
            if (char* const programName = (char*)ptr)
            {
                std::strncpy(programName, fProgramName, 23);
                programName[23] = '\0';
                return 1;
            }
            break;

        case effGetProgramNameIndexed:
            if (char* const programName = (char*)ptr)
            {
                std::strncpy(programName, fProgramName, 23);
                programName[23] = '\0';
                return 1;
            }
            break;

        case effGetParamDisplay:
            CARLA_SAFE_ASSERT_RETURN(index >= 0, 0);
#ifndef CARLA_VST_SHELL
            CARLA_SAFE_ASSERT_RETURN(index < kNumParameters, 0);
#endif

            if (char* const cptr = (char*)ptr)
            {
                const uint32_t uindex = static_cast<uint32_t>(index);
                CARLA_SAFE_ASSERT_RETURN(uindex < fDescriptor->paramIns, 0);

                const NativeParameter* const param = fDescriptor->get_parameter_info(fHandle, uindex);
                CARLA_SAFE_ASSERT_RETURN(param != nullptr, 0);

                float paramValue = fDescriptor->get_parameter_value(fHandle, uindex);

                if (param->hints & NATIVE_PARAMETER_IS_BOOLEAN)
                {
                    const NativeParameterRanges& ranges(param->ranges);
                    const float midRange = ranges.min + (ranges.max - ranges.min) / 2.0f;

                    paramValue = paramValue > midRange ? ranges.max : ranges.min;
                }
                else if (param->hints & NATIVE_PARAMETER_IS_INTEGER)
                {
                    paramValue = std::round(paramValue);
                }

                for (uint32_t i = 0; i < param->scalePointCount; ++i)
                {
                    const NativeParameterScalePoint& scalePoint(param->scalePoints[uindex]);

                    if (carla_isNotEqual(paramValue, scalePoint.value))
                        continue;

                    std::strncpy(cptr, scalePoint.label, 23);
                    cptr[23] = '\0';
                    return 1;
                }

                if (param->hints & NATIVE_PARAMETER_IS_INTEGER)
                {
                    std::snprintf(cptr, 23, "%d%s%s",
                                  static_cast<int>(paramValue),
                                  param->unit != nullptr && param->unit[0] != '\0' ? " " : "",
                                  param->unit != nullptr && param->unit[0] != '\0' ? param->unit : "");
                    cptr[23] = '\0';
                }
                else
                {
                    std::snprintf(cptr, 23, "%.12g%s%s",
                                  static_cast<double>(paramValue),
                                  param->unit != nullptr && param->unit[0] != '\0' ? " " : "",
                                  param->unit != nullptr && param->unit[0] != '\0' ? param->unit : "");
                    cptr[23] = '\0';
                }

                return 1;
            }
            break;

        case effGetParamName:
            CARLA_SAFE_ASSERT_RETURN(index >= 0, 0);
#ifndef CARLA_VST_SHELL
            CARLA_SAFE_ASSERT_RETURN(index < kNumParameters, 0);
#endif

            if (char* const cptr = (char*)ptr)
            {
                const uint32_t uindex = static_cast<uint32_t>(index);
                CARLA_SAFE_ASSERT_RETURN(uindex < fDescriptor->paramIns, 0);

                const NativeParameter* const param = fDescriptor->get_parameter_info(fHandle, uindex);
                CARLA_SAFE_ASSERT_RETURN(param != nullptr, 0);

                std::strncpy(cptr, param->name, 15);
                cptr[15] = '\0';
                return 1;
            }
            return 0;

        case effSetSampleRate:
            CARLA_SAFE_ASSERT_RETURN(opt > 0.0f, 0);

            if (carla_isEqual(fSampleRate, static_cast<double>(opt)))
                return 0;

            fSampleRate = opt;

            if (fDescriptor->dispatcher != nullptr)
                fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_SAMPLE_RATE_CHANGED, 0, 0, nullptr, opt);
            break;

        case effSetBlockSize:
            CARLA_SAFE_ASSERT_RETURN(value > 0, 0);

            if (fBufferSize == static_cast<uint32_t>(value))
                return 0;

            fBufferSize = static_cast<uint32_t>(value);

            if (fDescriptor->dispatcher != nullptr)
                fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_BUFFER_SIZE_CHANGED, 0, value, nullptr, 0.0f);
            break;

        case effMainsChanged:
            if (value != 0)
            {
                fMidiEventCount = 0;
                carla_zeroStruct(fTimeInfo);

                // tell host we want MIDI events
                if (fDescriptor->midiIns > 0)
                    hostCallback(audioMasterWantMidi);

                // deactivate for possible changes
                if (fDescriptor->deactivate != nullptr && fIsActive)
                    fDescriptor->deactivate(fHandle);

                // check if something changed
                const uint32_t bufferSize = static_cast<uint32_t>(hostCallback(audioMasterGetBlockSize));
                const double   sampleRate = static_cast<double>(hostCallback(audioMasterGetSampleRate));

                if (bufferSize != 0 && fBufferSize != bufferSize && (fHostType != kHostTypeArdour || fBufferSize == 0))
                {
                    fBufferSize = bufferSize;

                    if (fDescriptor->dispatcher != nullptr)
                        fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_BUFFER_SIZE_CHANGED, 0, bufferSize, nullptr, 0.0f);
                }

                if (carla_isNotZero(sampleRate) && carla_isNotEqual(fSampleRate, sampleRate))
                {
                    fSampleRate = sampleRate;

                    if (fDescriptor->dispatcher != nullptr)
                        fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_SAMPLE_RATE_CHANGED, 0, 0, nullptr, (float)sampleRate);
                }

                if (fDescriptor->activate != nullptr)
                    fDescriptor->activate(fHandle);

                fIsActive = true;
            }
            else
            {
                CARLA_SAFE_ASSERT_BREAK(fIsActive);

                if (fDescriptor->deactivate != nullptr)
                    fDescriptor->deactivate(fHandle);

                fIsActive = false;
            }
            break;

        case effEditGetRect:
            *(ERect**)ptr = &fVstRect;
            ret = 1;
            break;

        case effEditOpen:
            if (fDescriptor->ui_show != nullptr)
            {
                if (kIsUsingUILauncher)
                {
                    destoryUILauncher(fUiLauncher);
                    fUiLauncher = createUILauncher((uintptr_t)ptr, fDescriptor, fHandle);
                }
                else
                {
                    char strBuf[0xff+1];
                    std::snprintf(strBuf, 0xff, P_INTPTR, (intptr_t)ptr);
                    strBuf[0xff] = '\0';

                    // set CARLA_PLUGIN_EMBED_WINID for external process
                    carla_setenv("CARLA_PLUGIN_EMBED_WINID", strBuf);

                    // show UI now
                    fDescriptor->ui_show(fHandle, true);

                    // reset CARLA_PLUGIN_EMBED_WINID just in case
                    carla_setenv("CARLA_PLUGIN_EMBED_WINID", "0");
                }
                ret = 1;
            }
            break;

        case effEditClose:
            if (fDescriptor->ui_show != nullptr)
            {
                if (kIsUsingUILauncher)
                {
                    destoryUILauncher(fUiLauncher);
                    fUiLauncher = nullptr;
                }
                else
                {
                    fDescriptor->ui_show(fHandle, false);
                }
                ret = 1;
            }
            break;

        case effEditIdle:
            if (fUiLauncher != nullptr)
                idleUILauncher(fUiLauncher);
            if (fDescriptor->ui_idle != nullptr)
                fDescriptor->ui_idle(fHandle);
            break;

        case effGetChunk:
            if (ptr == nullptr || fDescriptor->get_state == nullptr)
                return 0;

            if (fStateChunk != nullptr)
                std::free(fStateChunk);

            fStateChunk = fDescriptor->get_state(fHandle);

            if (fStateChunk == nullptr)
                return 0;

            ret = static_cast<intptr_t>(std::strlen(fStateChunk)+1);
            *(void**)ptr = fStateChunk;
            break;

        case effSetChunk:
            if (value <= 0 || fDescriptor->set_state == nullptr)
                return 0;
            if (value == 1)
                return 1;

            if (const char* const state = (const char*)ptr)
            {
                fDescriptor->set_state(fHandle, state);
                ret = 1;
            }
            break;

        case effProcessEvents:
            if (! fIsActive)
            {
                // host has not activated the plugin yet, nasty!
                vst_dispatcher(effMainsChanged, 0, 1, nullptr, 0.0f);
            }

            if (const VstEvents* const events = (const VstEvents*)ptr)
            {
                if (events->numEvents == 0)
                    break;

                for (int i=0, count=events->numEvents; i < count; ++i)
                {
                    const VstMidiEvent* const vstMidiEvent((const VstMidiEvent*)events->events[i]);

                    if (vstMidiEvent == nullptr)
                        break;
                    if (vstMidiEvent->type != kVstMidiType || vstMidiEvent->deltaFrames < 0)
                        continue;
                    if (fMidiEventCount >= kMaxMidiEvents)
                        break;

                    const uint32_t j(fMidiEventCount++);

                    fMidiEvents[j].port = 0;
                    fMidiEvents[j].time = static_cast<uint32_t>(vstMidiEvent->deltaFrames);
                    fMidiEvents[j].size = 3;

                    for (uint32_t k=0; k<3; ++k)
                        fMidiEvents[j].data[k] = static_cast<uint8_t>(vstMidiEvent->midiData[k]);
                }
            }
            break;

        case effCanBeAutomated:
            ret = 1;
            break;

        case effCanDo:
            if (const char* const canDo = (const char*)ptr)
            {
                if (std::strcmp(canDo, "receiveVstEvents") == 0 || std::strcmp(canDo, "receiveVstMidiEvent") == 0)
                {
                    if (fDescriptor->midiIns == 0)
                        return -1;
                    return 1;
                }
                if (std::strcmp(canDo, "sendVstEvents") == 0 || std::strcmp(canDo, "sendVstMidiEvent") == 0)
                {
                    if (fDescriptor->midiOuts == 0)
                        return -1;
                    return 1;
                }
                if (std::strcmp(canDo, "receiveVstTimeInfo") == 0)
                    return 1;
            }
            break;
        }

        return ret;
    }

    float vst_getParameter(const int32_t index)
    {
        CARLA_SAFE_ASSERT_RETURN(index >= 0, 0.0f);

        const uint32_t uindex = static_cast<uint32_t>(index);
        CARLA_SAFE_ASSERT_RETURN(uindex < fDescriptor->paramIns, 0.0f);

        const NativeParameter* const param = fDescriptor->get_parameter_info(fHandle, uindex);
        CARLA_SAFE_ASSERT_RETURN(param != nullptr, 0);

        const float realValue = fDescriptor->get_parameter_value(fHandle, uindex);

        return (realValue - param->ranges.min) / (param->ranges.max - param->ranges.min);
    }

    void vst_setParameter(const int32_t index, const float value)
    {
        CARLA_SAFE_ASSERT_RETURN(index >= 0,);

        const uint32_t uindex = static_cast<uint32_t>(index);
        CARLA_SAFE_ASSERT_RETURN(uindex < fDescriptor->paramIns,);

        const NativeParameter* const param = fDescriptor->get_parameter_info(fHandle, uindex);
        CARLA_SAFE_ASSERT_RETURN(param != nullptr,);

        float realValue;

        if (param->hints & NATIVE_PARAMETER_IS_BOOLEAN)
        {
            realValue = value > 0.5f ? param->ranges.max : param->ranges.min;
        }
        else
        {
            realValue = param->ranges.min + ((param->ranges.max - param->ranges.min) * value);

            if (param->hints & NATIVE_PARAMETER_IS_INTEGER)
                realValue = std::round(realValue);
        }

        fDescriptor->set_parameter_value(fHandle, uindex, realValue);
    }

    // FIXME for v3.0, use const for the input buffer
    void vst_processReplacing(float** const inputs, float** const outputs, const int32_t sampleFrames)
    {
        if (sampleFrames <= 0)
            return;

        if (fHostType == kHostTypeBitwig && static_cast<int32_t>(fBufferSize) != sampleFrames)
        {
            // deactivate first if needed
            if (fIsActive && fDescriptor->deactivate != nullptr)
                fDescriptor->deactivate(fHandle);

            fBufferSize = static_cast<uint32_t>(sampleFrames);

            if (fDescriptor->dispatcher != nullptr)
                fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_BUFFER_SIZE_CHANGED, 0, sampleFrames, nullptr, 0.0f);

            // activate again
            if (fDescriptor->activate != nullptr)
                fDescriptor->activate(fHandle);

            fIsActive = true;
        }

        if (! fIsActive)
        {
            // host has not activated the plugin yet, nasty!
            vst_dispatcher(effMainsChanged, 0, 1, nullptr, 0.0f);
        }

        static const int kWantVstTimeFlags = kVstTransportPlaying|kVstPpqPosValid|kVstTempoValid|kVstTimeSigValid;

        if (const VstTimeInfo* const vstTimeInfo = (const VstTimeInfo*)hostCallback(audioMasterGetTime, 0, kWantVstTimeFlags))
        {
            fTimeInfo.frame     = static_cast<uint64_t>(std::max(0.0, vstTimeInfo->samplePos));
            fTimeInfo.playing   =  (vstTimeInfo->flags & kVstTransportPlaying);
            fTimeInfo.bbt.valid = ((vstTimeInfo->flags & kVstTempoValid) != 0 || (vstTimeInfo->flags & kVstTimeSigValid) != 0);

            // ticksPerBeat is not possible with VST
            fTimeInfo.bbt.ticksPerBeat = 960.0;

            if (vstTimeInfo->flags & kVstTempoValid)
                fTimeInfo.bbt.beatsPerMinute = vstTimeInfo->tempo;
            else
                fTimeInfo.bbt.beatsPerMinute = 120.0;

            if ((vstTimeInfo->flags & kVstPpqPosValid) != 0 && (vstTimeInfo->flags & kVstTimeSigValid) != 0)
            {
                const double ppqPos    = std::abs(vstTimeInfo->ppqPos);
                const int    ppqPerBar = vstTimeInfo->timeSigNumerator * 4 / vstTimeInfo->timeSigDenominator;
                const double barBeats  = (std::fmod(ppqPos, ppqPerBar) / ppqPerBar) * vstTimeInfo->timeSigNumerator;
                const double rest      =  std::fmod(barBeats, 1.0);

                fTimeInfo.bbt.bar         = static_cast<int32_t>(ppqPos) / ppqPerBar + 1;
                fTimeInfo.bbt.beat        = static_cast<int32_t>(barBeats - rest + 0.5) + 1;
                fTimeInfo.bbt.tick        = rest * fTimeInfo.bbt.ticksPerBeat;
                fTimeInfo.bbt.beatsPerBar = static_cast<float>(vstTimeInfo->timeSigNumerator);
                fTimeInfo.bbt.beatType    = static_cast<float>(vstTimeInfo->timeSigDenominator);

                if (vstTimeInfo->ppqPos < 0.0)
                {
                    fTimeInfo.bbt.bar  = std::max(1, fTimeInfo.bbt.bar-1);
                    fTimeInfo.bbt.beat = std::max(1, vstTimeInfo->timeSigNumerator - fTimeInfo.bbt.beat + 1);
                    fTimeInfo.bbt.tick = std::max(0.0, fTimeInfo.bbt.ticksPerBeat - fTimeInfo.bbt.tick - 1);
                }
            }
            else
            {
                fTimeInfo.bbt.bar         = 1;
                fTimeInfo.bbt.beat        = 1;
                fTimeInfo.bbt.tick        = 0.0;
                fTimeInfo.bbt.beatsPerBar = 4.0f;
                fTimeInfo.bbt.beatType    = 4.0f;
            }

#if 0
            // for testing bad host values
            static double last = -99999.0;
            if (carla_isNotEqual(vstTimeInfo->samplePos, last))
            {
                last = vstTimeInfo->samplePos;
                carla_stdout("time info %f %f %lu | %i %i",
                             vstTimeInfo->samplePos, vstTimeInfo->ppqPos, fTimeInfo.frame,
                             vstTimeInfo->timeSigNumerator, vstTimeInfo->timeSigDenominator);
            }
#endif

            fTimeInfo.bbt.barStartTick = fTimeInfo.bbt.ticksPerBeat *
                                         static_cast<double>(fTimeInfo.bbt.beatsPerBar) *
                                         (fTimeInfo.bbt.bar - 1);
        }

        fMidiOutEvents.numEvents = 0;

        if (fHandle != nullptr)
            fDescriptor->process(fHandle,
                                 inputs, outputs, static_cast<uint32_t>(sampleFrames),
                                 fMidiEvents, fMidiEventCount);

        fMidiEventCount = 0;

        if (fMidiOutEvents.numEvents > 0)
            hostCallback(audioMasterProcessEvents, 0, 0, &fMidiOutEvents, 0.0f);
    }

protected:
    // -------------------------------------------------------------------

    bool handleWriteMidiEvent(const NativeMidiEvent* const event)
    {
        CARLA_SAFE_ASSERT_RETURN(fDescriptor->midiOuts > 0, false);
        CARLA_SAFE_ASSERT_RETURN(event != nullptr, false);
        CARLA_SAFE_ASSERT_RETURN(event->data[0] != 0, false);

        if (fMidiOutEvents.numEvents >= static_cast<int32_t>(kMaxMidiEvents))
        {
            // send current events
            hostCallback(audioMasterProcessEvents, 0, 0, &fMidiOutEvents, 0.0f);

            // clear
            fMidiOutEvents.numEvents = 0;
        }

        VstMidiEvent& vstMidiEvent(fMidiOutEvents.mdata[fMidiOutEvents.numEvents++]);

        vstMidiEvent.type     = kVstMidiType;
        vstMidiEvent.byteSize = kVstMidiEventSize;

        uint8_t i=0;
        for (; i<event->size; ++i)
            vstMidiEvent.midiData[i] = static_cast<char>(event->data[i]);
        for (; i<4; ++i)
            vstMidiEvent.midiData[i] = 0;

        return true;
    }

    void handleUiParameterChanged(const uint32_t index, const float value) const
    {
        const NativeParameter* const param = fDescriptor->get_parameter_info(fHandle, index);
        CARLA_SAFE_ASSERT_RETURN(param != nullptr,);

        const float normalizedValue = (value - param->ranges.min) / (param->ranges.max - param->ranges.min);

        hostCallback(audioMasterAutomate, static_cast<int32_t>(index), 0, nullptr, normalizedValue);
    }

    void handleUiParameterTouch(const uint32_t index, const bool touch) const
    {
        hostCallback(touch ? audioMasterBeginEdit : audioMasterEndEdit, static_cast<int32_t>(index));
    }

    void handleUiResize(const int16_t width, const int16_t height)
    {
        if (kIsUsingUILauncher)
            return;
        fVstRect.right = width;
        fVstRect.bottom = height;
        hostCallback(audioMasterSizeWindow, width, height);
    }

    void handleUiCustomDataChanged(const char* const /*key*/, const char* const /*value*/) const
    {
    }

    void handleUiClosed()
    {
    }

    const char* handleUiOpenFile(const bool /*isDir*/, const char* const /*title*/, const char* const /*filter*/) const
    {
        // TODO
        return nullptr;
    }

    const char* handleUiSaveFile(const bool /*isDir*/, const char* const /*title*/, const char* const /*filter*/) const
    {
        // TODO
        return nullptr;
    }

    intptr_t handleDispatcher(const NativeHostDispatcherOpcode opcode, const int32_t index, const intptr_t value, void* const ptr, const float opt)
    {
        carla_debug("NativePlugin::handleDispatcher(%i, %i, " P_INTPTR ", %p, %f)",
                    opcode, index, value, ptr, static_cast<double>(opt));

        switch (opcode)
        {
        case NATIVE_HOST_OPCODE_NULL:
        case NATIVE_HOST_OPCODE_UPDATE_PARAMETER:
        case NATIVE_HOST_OPCODE_UPDATE_MIDI_PROGRAM:
        case NATIVE_HOST_OPCODE_RELOAD_PARAMETERS:
        case NATIVE_HOST_OPCODE_RELOAD_MIDI_PROGRAMS:
        case NATIVE_HOST_OPCODE_UI_UNAVAILABLE:
        case NATIVE_HOST_OPCODE_INTERNAL_PLUGIN:
        case NATIVE_HOST_OPCODE_QUEUE_INLINE_DISPLAY:
        case NATIVE_HOST_OPCODE_REQUEST_IDLE:
        case NATIVE_HOST_OPCODE_GET_FILE_PATH:
        case NATIVE_HOST_OPCODE_PREVIEW_BUFFER_DATA:
            // nothing
            break;

        case NATIVE_HOST_OPCODE_RELOAD_ALL:
            hostCallback(audioMasterUpdateDisplay);
            break;

        case NATIVE_HOST_OPCODE_HOST_IDLE:
            hostCallback(audioMasterIdle);
            break;

        case NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER:
            CARLA_SAFE_ASSERT_RETURN(index >= 0, 0);
            handleUiParameterTouch(static_cast<uint32_t>(index), value != 0);
            break;

        case NATIVE_HOST_OPCODE_UI_RESIZE:
            CARLA_SAFE_ASSERT_RETURN(index > 0 && index < INT16_MAX, 0);
            CARLA_SAFE_ASSERT_RETURN(value > 0 && value < INT16_MAX, 0);
            handleUiResize(static_cast<int16_t>(index), static_cast<int16_t>(value));
            break;
        }

        // unused for now
        return 0;

        (void)ptr; (void)opt;
    }

private:
    // VST stuff
    AEffect* const fEffect;

    // Native data
    NativePluginHandle   fHandle;
    NativeHostDescriptor fHost;
    const NativePluginDescriptor* const fDescriptor;

    // VST host data
    uint32_t fBufferSize;
    double   fSampleRate;

    // Temporary data
    bool            fIsActive;
    uint32_t        fMidiEventCount;
    NativeMidiEvent fMidiEvents[kMaxMidiEvents];
    char            fProgramName[32+1];
    NativeTimeInfo  fTimeInfo;
    ERect           fVstRect;

    // UI button
    CarlaUILauncher* fUiLauncher;

    // Host data
    enum HostType {
        kHostTypeNull = 0,
        kHostTypeArdour,
        kHostTypeBitwig
    };
    HostType fHostType;

    // host callback
    intptr_t hostCallback(const int32_t opcode,
                          const int32_t index = 0,
                          const intptr_t value = 0,
                          void* const ptr = nullptr,
                          const float opt = 0.0f) const
    {
        return VSTAudioMaster(fEffect, opcode, index, value, ptr, opt);
    }

    struct FixedVstEvents {
        int32_t numEvents;
        intptr_t reserved;
        VstEvent* data[kMaxMidiEvents];
        VstMidiEvent mdata[kMaxMidiEvents];

        FixedVstEvents()
            : numEvents(0),
              reserved(0)
        {
            for (uint32_t i=0; i<kMaxMidiEvents; ++i)
                data[i] = (VstEvent*)&mdata[i];
            carla_zeroStructs(mdata, kMaxMidiEvents);
        }

        CARLA_DECLARE_NON_COPYABLE(FixedVstEvents);
    } fMidiOutEvents;

    char* fStateChunk;

    // -------------------------------------------------------------------

    #define handlePtr ((NativePlugin*)handle)

    static uint32_t host_get_buffer_size(NativeHostHandle handle)
    {
        return handlePtr->fBufferSize;
    }

    static double host_get_sample_rate(NativeHostHandle handle)
    {
        return handlePtr->fSampleRate;
    }

    static bool host_is_offline(NativeHostHandle /*handle*/)
    {
        // TODO
        return false;
    }

    static const NativeTimeInfo* host_get_time_info(NativeHostHandle handle)
    {
        return &(handlePtr->fTimeInfo);
    }

    static bool host_write_midi_event(NativeHostHandle handle, const NativeMidiEvent* event)
    {
        return handlePtr->handleWriteMidiEvent(event);
    }

    static void host_ui_parameter_changed(NativeHostHandle handle, uint32_t index, float value)
    {
        handlePtr->handleUiParameterChanged(index, value);
    }

    static void host_ui_custom_data_changed(NativeHostHandle handle, const char* key, const char* value)
    {
        handlePtr->handleUiCustomDataChanged(key, value);
    }

    static void host_ui_closed(NativeHostHandle handle)
    {
        handlePtr->handleUiClosed();
    }

    static const char* host_ui_open_file(NativeHostHandle handle, bool isDir, const char* title, const char* filter)
    {
        return handlePtr->handleUiOpenFile(isDir, title, filter);
    }

    static const char* host_ui_save_file(NativeHostHandle handle, bool isDir, const char* title, const char* filter)
    {
        return handlePtr->handleUiSaveFile(isDir, title, filter);
    }

    static intptr_t host_dispatcher(NativeHostHandle handle, NativeHostDispatcherOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt)
    {
        return handlePtr->handleDispatcher(opcode, index, value, ptr, opt);
    }

    #undef handlePtr

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NativePlugin)
};

// -----------------------------------------------------------------------

#define validObject  effect != nullptr && effect->object != nullptr
#define validPlugin  effect != nullptr && effect->object != nullptr && ((VstObject*)effect->object)->plugin != nullptr
#define vstObjectPtr (VstObject*)effect->object
#define pluginPtr    (vstObjectPtr)->plugin

intptr_t vst_dispatcherCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt)
{
    // handle base opcodes
    switch (opcode)
    {
    case effOpen:
        if (VstObject* const obj = vstObjectPtr)
        {
            // this must always be valid
            CARLA_SAFE_ASSERT_RETURN(obj->audioMaster != nullptr, 0);

            // some hosts call effOpen twice
            if (obj->plugin != nullptr)
                return 1;

            d_lastBufferSize = static_cast<uint32_t>(VSTAudioMaster(effect, audioMasterGetBlockSize, 0, 0, nullptr, 0.0f));
            d_lastSampleRate = static_cast<double>(VSTAudioMaster(effect, audioMasterGetSampleRate, 0, 0, nullptr, 0.0f));

            // some hosts are not ready at this point or return 0 buffersize/samplerate
            if (d_lastBufferSize == 0)
                d_lastBufferSize = 2048;
            if (d_lastSampleRate <= 0.0)
                d_lastSampleRate = 44100.0;

            const NativePluginDescriptor* pluginDesc = nullptr;
            PluginListManager& plm(PluginListManager::getInstance());

           #ifdef CARLA_VST_SHELL
            if (effect->uniqueID == 0)
                effect->uniqueID = kShellUniqueID;

            if (effect->uniqueID == kShellUniqueID)
            {
                // first open for discovery, nothing to do
                effect->numParams   = 0;
                effect->numPrograms = 0;
                effect->numInputs   = 0;
                effect->numOutputs  = 0;
                return 1;
            }

            const int32_t plugIndex = effect->uniqueID - kShellUniqueID - 1;
            CARLA_SAFE_ASSERT_RETURN(plugIndex >= 0, 0);

            pluginDesc = plm.descs.getAt(static_cast<size_t>(plugIndex), nullptr);

           #else // CARLA_VST_SHELL

           # if defined(CARLA_PLUGIN_64CH)
            const char* const pluginLabel = "carlapatchbay64";
           # elif defined(CARLA_PLUGIN_32CH)
            const char* const pluginLabel = "carlapatchbay32";
           # elif defined(CARLA_PLUGIN_16CH)
            const char* const pluginLabel = "carlapatchbay16";
           # elif CARLA_PLUGIN_PATCHBAY
            const char* const pluginLabel = "carlapatchbay";
           # else
            const char* const pluginLabel = "carlarack";
           # endif

            for (LinkedList<const NativePluginDescriptor*>::Itenerator it = plm.descs.begin2(); it.valid(); it.next())
            {
                const NativePluginDescriptor* const& tmpDesc(it.getValue(nullptr));
                CARLA_SAFE_ASSERT_CONTINUE(tmpDesc != nullptr);

                if (std::strcmp(tmpDesc->label, pluginLabel) == 0)
                {
                    pluginDesc = tmpDesc;
                    break;
                }
            }
           #endif // CARLA_VST_SHELL

            CARLA_SAFE_ASSERT_RETURN(pluginDesc != nullptr, 0);

           #ifdef CARLA_VST_SHELL
            effect->numPrograms = 1;
            effect->numParams   = static_cast<int>(pluginDesc->paramIns);
            effect->numInputs   = static_cast<int>(pluginDesc->audioIns);
            effect->numOutputs  = static_cast<int>(pluginDesc->audioOuts);

            if (pluginDesc->hints & NATIVE_PLUGIN_HAS_UI)
                effect->flags |= effFlagsHasEditor;
            else
                effect->flags &= ~effFlagsHasEditor;

            /* carla as plugin always has NATIVE_PLUGIN_IS_SYNTH set
            if (pluginDesc->hints & NATIVE_PLUGIN_IS_SYNTH)
                effect->flags |= effFlagsIsSynth;
            else
                effect->flags &= ~effFlagsIsSynth;
            */
           #endif // CARLA_VST_SHELL

           #if CARLA_PLUGIN_SYNTH
            // override if requested
            effect->flags |= effFlagsIsSynth;
           #endif

            obj->plugin = new NativePlugin(effect, pluginDesc);
            return 1;
        }
        return 0;

    case effClose:
        if (VstObject* const obj = vstObjectPtr)
        {
            NativePlugin* const plugin(obj->plugin);

            if (plugin != nullptr)
            {
                obj->plugin = nullptr;
                delete plugin;
            }

#if 0
            /* This code invalidates the object created in VSTPluginMain
             * Probably not safe against all hosts */
            obj->audioMaster = nullptr;
            effect->object = nullptr;
            delete obj;
#endif

            return 1;
        }
        //delete effect;
        return 0;

    case effGetPlugCategory:
#ifdef CARLA_VST_SHELL
        if (validPlugin)
        {
           #if CARLA_PLUGIN_SYNTH
            return kPlugCategSynth;
           #else
            const NativePluginDescriptor* const desc = pluginPtr->getDescriptor();
            return desc->category == NATIVE_PLUGIN_CATEGORY_SYNTH ? kPlugCategSynth : kPlugCategEffect;
           #endif
        }

        return kPlugCategShell;
#elif CARLA_PLUGIN_SYNTH
        return kPlugCategSynth;
#else
        return kPlugCategEffect;
#endif

    case effGetEffectName:
        if (char* const cptr = (char*)ptr)
        {
           #if defined(CARLA_VST_SHELL)
            if (validPlugin)
            {
                const NativePluginDescriptor* const desc = pluginPtr->getDescriptor();
               #if CARLA_PLUGIN_SYNTH
                std::strncpy(cptr, desc->name, 32);
               #else
                std::snprintf(cptr, 32, "%s FX", desc->name);
               #endif
            }
            else
            {
                std::strncpy(cptr, "Carla-VstShell", 32);
            }
           #elif defined(CARLA_PLUGIN_64CH)
            std::strncpy(cptr, "Carla-Patchbay64", 32);
           #elif defined(CARLA_PLUGIN_32CH)
            std::strncpy(cptr, "Carla-Patchbay32", 32);
           #elif defined(CARLA_PLUGIN_16CH)
            std::strncpy(cptr, "Carla-Patchbay16", 32);
           #elif CARLA_PLUGIN_PATCHBAY
           #  if CARLA_PLUGIN_SYNTH
            std::strncpy(cptr, "Carla-Patchbay", 32);
           #  else
            std::strncpy(cptr, "Carla-PatchbayFX", 32);
           #  endif
           #else // Rack mode
           #  if CARLA_PLUGIN_SYNTH
            std::strncpy(cptr, "Carla-Rack", 32);
           #  else
            std::strncpy(cptr, "Carla-RackFX", 32);
           #  endif
           #endif
            return 1;
        }
        return 0;

    case effGetVendorString:
        if (char* const cptr = (char*)ptr)
        {
           #ifdef CARLA_VST_SHELL
            if (validPlugin)
            {
                const NativePluginDescriptor* const desc = pluginPtr->getDescriptor();
                std::strncpy(cptr, desc->maker, 32);
            }
            else
           #endif
            std::strncpy(cptr, "falkTX", 32);
            return 1;
        }
        return 0;

    case effGetProductString:
        if (char* const cptr = (char*)ptr)
        {
           #if defined(CARLA_VST_SHELL)
            if (validPlugin)
            {
                const NativePluginDescriptor* const desc = pluginPtr->getDescriptor();
               #if CARLA_PLUGIN_SYNTH
                std::strncpy(cptr, desc->label, 32);
               #else
                std::snprintf(cptr, 32, "%sFX", desc->label);
               #endif
            }
            else
            {
                std::strncpy(cptr, "CarlaVstShell", 32);
            }
           #elif defined(CARLA_PLUGIN_64CH)
            std::strncpy(cptr, "CarlaPatchbay64", 32);
           #elif defined(CARLA_PLUGIN_32CH)
            std::strncpy(cptr, "CarlaPatchbay32", 32);
           #elif defined(CARLA_PLUGIN_16CH)
            std::strncpy(cptr, "CarlaPatchbay16", 32);
           #elif CARLA_PLUGIN_PATCHBAY
           #  if CARLA_PLUGIN_SYNTH
            std::strncpy(cptr, "CarlaPatchbay", 32);
           #  else
            std::strncpy(cptr, "CarlaPatchbayFX", 32);
           #  endif
           #else
           #  if CARLA_PLUGIN_SYNTH
            std::strncpy(cptr, "CarlaRack", 32);
           #  else
            std::strncpy(cptr, "CarlaRackFX", 32);
           #  endif
           #endif
            return 1;
        }
        return 0;

    case effGetVendorVersion:
        return CARLA_VERSION_HEX;

    case effGetVstVersion:
        return kVstVersion;

#ifdef CARLA_VST_SHELL
    case effShellGetNextPlugin:
        if (char* const cptr = (char*)ptr)
        {
            CARLA_SAFE_ASSERT_RETURN(effect != nullptr, 0);
            CARLA_SAFE_ASSERT_RETURN(effect->uniqueID >= kShellUniqueID, 0);

            PluginListManager& plm(PluginListManager::getInstance());

            for (;;)
            {
                const uint index2 = static_cast<uint>(effect->uniqueID - kShellUniqueID);

                if (index2 >= plm.descs.count())
                {
                    effect->uniqueID = kShellUniqueID;
                    return 0;
                }

                const NativePluginDescriptor* const desc = plm.descs.getAt(index2, nullptr);
                CARLA_SAFE_ASSERT_RETURN(desc != nullptr, 0);

                ++effect->uniqueID;

                if (desc->midiIns > 1 || desc->midiOuts > 1)
                    continue;

               #if CARLA_PLUGIN_SYNTH
                std::strncpy(cptr, desc->label, 32);
               #else
                std::snprintf(cptr, 32, "%sFX", desc->label);
               #endif

                return effect->uniqueID;
            }
        }
#endif
    };

    // handle advanced opcodes
    if (validPlugin)
        return pluginPtr->vst_dispatcher(opcode, index, value, ptr, opt);

    return 0;
}

float vst_getParameterCallback(AEffect* effect, int32_t index)
{
    if (validPlugin)
        return pluginPtr->vst_getParameter(index);
    return 0.0f;
}

void vst_setParameterCallback(AEffect* effect, int32_t index, float value)
{
    if (validPlugin)
        pluginPtr->vst_setParameter(index, value);
}

void vst_processCallback(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames)
{
    if (validPlugin)
        pluginPtr->vst_processReplacing(inputs, outputs, sampleFrames);
}

void vst_processReplacingCallback(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames)
{
    if (validPlugin)
        pluginPtr->vst_processReplacing(inputs, outputs, sampleFrames);
}

#undef pluginPtr
#undef validObject
#undef validPlugin
#undef vstObjectPtr

// -----------------------------------------------------------------------

const AEffect* VSTPluginMainInit(AEffect* const effect)
{
   #if defined(CARLA_VST_SHELL)
    if (const intptr_t uniqueID = VSTAudioMaster(effect, audioMasterCurrentId, 0, 0, nullptr, 0.0f))
        effect->uniqueID = static_cast<int>(uniqueID);
    else
        effect->uniqueID = kShellUniqueID;
   #elif defined(CARLA_PLUGIN_64CH)
    effect->uniqueID = kBaseUniqueID+7;
   #elif defined(CARLA_PLUGIN_32CH)
    effect->uniqueID = kBaseUniqueID+6;
   #elif defined(CARLA_PLUGIN_16CH)
    effect->uniqueID = kBaseUniqueID+5;
   #elif CARLA_PLUGIN_PATCHBAY
   #  if CARLA_PLUGIN_SYNTH
    effect->uniqueID = kBaseUniqueID+4;
   #  else
    effect->uniqueID = kBaseUniqueID+3;
   #  endif
   #else
   #  if CARLA_PLUGIN_SYNTH
    effect->uniqueID = kBaseUniqueID+2;
   #  else
    effect->uniqueID = kBaseUniqueID+1;
   #  endif
   #endif

    // plugin fields
#ifndef CARLA_VST_SHELL
    effect->numParams   = kNumParameters;
    effect->numPrograms = 1;
# if defined(CARLA_PLUGIN_64CH)
    effect->numInputs   = 64;
    effect->numOutputs  = 64;
# elif defined(CARLA_PLUGIN_32CH)
    effect->numInputs   = 32;
    effect->numOutputs  = 32;
# elif defined(CARLA_PLUGIN_16CH)
    effect->numInputs   = 16;
    effect->numOutputs  = 16;
# else
    effect->numInputs   = 2;
    effect->numOutputs  = 2;
# endif
#endif

    // plugin flags
    effect->flags |= effFlagsCanReplacing;
    effect->flags |= effFlagsProgramChunks;
#ifndef CARLA_VST_SHELL
    effect->flags |= effFlagsHasEditor;
#endif
#if CARLA_PLUGIN_SYNTH
    effect->flags |= effFlagsIsSynth;
#endif

    return effect;

    // may be unused
    (void)kBaseUniqueID;
}

// -----------------------------------------------------------------------
