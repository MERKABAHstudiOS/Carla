﻿/*
 * Carla Plugin Host
 * Copyright (C) 2011-2014 Filipe Coelho <falktx@falktx.com>
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

#ifndef BUILD_BRIDGE
# error This file should not be compiled if not building bridge
#endif

#include "CarlaEngineInternal.hpp"
#include "CarlaPlugin.hpp"

#include "CarlaBackendUtils.hpp"
#include "CarlaBridgeUtils.hpp"
#include "CarlaMIDI.h"

#include "jackbridge/JackBridge.hpp"

#include <cerrno>
#include <ctime>

using juce::File;
using juce::String;

// -------------------------------------------------------------------

#ifdef JACKBRIDGE_EXPORT
bool jackbridge_is_ok() noexcept
{
    return true;
}
#endif

template<typename T>
bool jackbridge_shm_map2(char* shm, T*& value)
{
    value = (T*)jackbridge_shm_map(shm, sizeof(T));
    return (value != nullptr);
}

// -------------------------------------------------------------------

CARLA_BACKEND_START_NAMESPACE

// -------------------------------------------------------------------

struct BridgeAudioPool {
    CarlaString filename;
    float* data;
    char shm[32];

    BridgeAudioPool()
        : data(nullptr)
    {
        carla_zeroChar(shm, 32);
        jackbridge_shm_init(shm);
    }

    ~BridgeAudioPool()
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool attach()
    {
        jackbridge_shm_attach(shm, filename);

        return jackbridge_shm_is_valid(shm);
    }

    void clear()
    {
        filename.clear();

        data = nullptr;

        if (jackbridge_shm_is_valid(shm))
            jackbridge_shm_close(shm);
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeAudioPool)
};

// -------------------------------------------------------------------

struct BridgeRtControl : public CarlaRingBuffer<StackBuffer> {
    CarlaString filename;
    BridgeRtData* data;
    char shm[32];

    BridgeRtControl()
        : data(nullptr)
    {
        carla_zeroChar(shm, 32);
        jackbridge_shm_init(shm);
    }

    ~BridgeRtControl()
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool attach()
    {
        jackbridge_shm_attach(shm, filename);

        return jackbridge_shm_is_valid(shm);
    }

    void clear()
    {
        filename.clear();

        data = nullptr;

        if (jackbridge_shm_is_valid(shm))
            jackbridge_shm_close(shm);
    }

    bool mapData()
    {
        CARLA_SAFE_ASSERT(data == nullptr);

        if (jackbridge_shm_map2<BridgeRtData>(shm, data))
        {
            setRingBuffer(&data->ringBuffer, false);
            return true;
        }

        return false;
    }

    PluginBridgeRtOpcode readOpcode() noexcept
    {
        return static_cast<PluginBridgeRtOpcode>(readInt());
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeRtControl)
};

// -------------------------------------------------------------------

struct BridgeNonRtControl : public CarlaRingBuffer<BigStackBuffer> {
    CarlaString filename;
    BridgeNonRtData* data;
    char shm[32];

    BridgeNonRtControl()
        : CarlaRingBuffer<BigStackBuffer>(),
          data(nullptr)
    {
        carla_zeroChar(shm, 32);
        jackbridge_shm_init(shm);
    }

    ~BridgeNonRtControl()
    {
        // should be cleared by now
        CARLA_SAFE_ASSERT(data == nullptr);

        clear();
    }

    bool attach()
    {
        jackbridge_shm_attach(shm, filename);

        return jackbridge_shm_is_valid(shm);
    }

    void clear()
    {
        filename.clear();

        data = nullptr;

        if (jackbridge_shm_is_valid(shm))
            jackbridge_shm_close(shm);
    }

    bool mapData()
    {
        CARLA_SAFE_ASSERT(data == nullptr);

        if (jackbridge_shm_map2<BridgeNonRtData>(shm, data))
        {
            setRingBuffer(&data->ringBuffer, false);
            return true;
        }

        return false;
    }

    PluginBridgeNonRtOpcode readOpcode() noexcept
    {
        return static_cast<PluginBridgeNonRtOpcode>(readInt());
    }

    CARLA_DECLARE_NON_COPY_STRUCT(BridgeNonRtControl)
};

// -------------------------------------------------------------------

class CarlaEngineBridge : public CarlaEngine,
                          public CarlaThread
{
public:
    CarlaEngineBridge(const char* const audioPoolBaseName, const char* const rtBaseName, const char* const nonRtBaseName)
        : CarlaEngine(),
          CarlaThread("CarlaEngineBridge"),
          fIsRunning(false),
          fIsOffline(false)
    {
        carla_stdout("CarlaEngineBridge::CarlaEngineBridge(\"%s\", \"%s\", \"%s\")", audioPoolBaseName, rtBaseName, nonRtBaseName);

        fShmAudioPool.filename  = "/carla-bridge_shm_ap_";
        fShmAudioPool.filename += audioPoolBaseName;

        fShmRtControl.filename  = "/carla-bridge_shm_rt_";
        fShmRtControl.filename += rtBaseName;

        fShmNonRtControl.filename  = "/carla-bridge_shm_nonrt_";
        fShmNonRtControl.filename += nonRtBaseName;
    }

    ~CarlaEngineBridge() noexcept override
    {
        carla_debug("CarlaEngineBridge::~CarlaEngineBridge()");
    }

    // -------------------------------------
    // CarlaEngine virtual calls

    bool init(const char* const clientName) override
    {
        carla_debug("CarlaEngineBridge::init(\"%s\")", clientName);

        if (! fShmAudioPool.attach())
        {
            carla_stdout("Failed to attach to audio pool shared memory");
            return false;
        }

        if (! fShmRtControl.attach())
        {
            clear();
            carla_stdout("Failed to attach to rt control shared memory");
            return false;
        }

        if (! fShmRtControl.mapData())
        {
            clear();
            carla_stdout("Failed to map rt control shared memory");
            return false;
        }

        if (! fShmNonRtControl.attach())
        {
            clear();
            carla_stdout("Failed to attach to non-rt control shared memory");
            return false;
        }

        if (! fShmNonRtControl.mapData())
        {
            clear();
            carla_stdout("Failed to map non-rt control shared memory");
            return false;
        }

        PluginBridgeNonRtOpcode opcode;

        opcode = fShmNonRtControl.readOpcode();
        CARLA_SAFE_ASSERT_INT(opcode == kPluginBridgeNonRtNull, opcode);

        const uint32_t shmRtDataSize = fShmNonRtControl.readUInt();
        CARLA_SAFE_ASSERT_INT2(shmRtDataSize == sizeof(BridgeRtData), shmRtDataSize, sizeof(BridgeRtData));

        const uint32_t shmNonRtDataSize = fShmNonRtControl.readUInt();
        CARLA_SAFE_ASSERT_INT2(shmNonRtDataSize == sizeof(BridgeNonRtData), shmNonRtDataSize, sizeof(BridgeNonRtData));

        opcode = fShmNonRtControl.readOpcode();
        CARLA_SAFE_ASSERT_INT(opcode == kPluginBridgeNonRtSetBufferSize, opcode);
        pData->bufferSize = fShmNonRtControl.readUInt();

        opcode = fShmNonRtControl.readOpcode();
        CARLA_SAFE_ASSERT_INT(opcode == kPluginBridgeNonRtSetSampleRate, opcode);
        pData->sampleRate = fShmNonRtControl.readDouble();

        carla_stdout("Carla Client Info:");
        carla_stdout("  BufferSize: %i", pData->bufferSize);
        carla_stdout("  SampleRate: %g", pData->sampleRate);
        carla_stdout("  sizeof(BridgeRtData):    %i/" P_SIZE, shmRtDataSize,    sizeof(BridgeRtData));
        carla_stdout("  sizeof(BridgeNonRtData): %i/" P_SIZE, shmNonRtDataSize, sizeof(BridgeNonRtData));

        startThread();

        CarlaEngine::init(clientName);
        return true;
    }

    bool close() override
    {
        carla_debug("CarlaEnginePlugin::close()");
        CarlaEngine::close();

        stopThread(5000);
        clear();

        return true;
    }

    bool isRunning() const noexcept override
    {
        return isThreadRunning();
    }

    bool isOffline() const noexcept override
    {
        return fIsOffline;
    }

    EngineType getType() const noexcept override
    {
        return kEngineTypeBridge;
    }

    const char* getCurrentDriverName() const noexcept
    {
        return "Bridge";
    }

    void idle() noexcept override
    {
        CarlaEngine::idle();

        handleNonRtData();
    }

    // -------------------------------------------------------------------

    void clear()
    {
        fShmAudioPool.clear();
        fShmRtControl.clear();
        fShmNonRtControl.clear();
    }

    void handleNonRtData()
    {
        for (; fShmNonRtControl.isDataAvailableForReading();)
        {
            const PluginBridgeNonRtOpcode opcode(fShmNonRtControl.readOpcode());
            CarlaPlugin* const plugin(pData->plugins[0].plugin);

            if (opcode != kPluginBridgeNonRtPing)
                carla_stdout("CarlaEngineBridge::handleNonRtData() - got opcode: %s", PluginBridgeNonRtOpcode2str(opcode));

            switch (opcode)
            {
            case kPluginBridgeNonRtNull:
                break;

            case kPluginBridgeNonRtPing:
                oscSend_bridge_pong();
                break;

            case kPluginBridgeNonRtActivate:
                if (plugin != nullptr && plugin->isEnabled())
                    plugin->activate();
                break;

            case kPluginBridgeNonRtDeactivate:
                if (plugin != nullptr && plugin->isEnabled())
                    plugin->deactivate();
                break;

            case kPluginBridgeNonRtSetBufferSize: {
                const uint32_t bufferSize(fShmNonRtControl.readUInt());
                pData->bufferSize = bufferSize;
                bufferSizeChanged(bufferSize);
                break;
            }

            case kPluginBridgeNonRtSetSampleRate: {
                const double sampleRate(fShmNonRtControl.readDouble());
                pData->sampleRate = sampleRate;
                sampleRateChanged(sampleRate);
                break;
            }

            case kPluginBridgeNonRtSetOffline:
                fIsOffline = true;
                offlineModeChanged(true);
                break;

            case kPluginBridgeNonRtSetOnline:
                fIsOffline = false;
                offlineModeChanged(false);
                break;

            case kPluginBridgeNonRtSetParameterValue: {
                const uint32_t index(fShmNonRtControl.readUInt());
                const float    value(fShmNonRtControl.readFloat());

                if (plugin != nullptr && plugin->isEnabled())
                    plugin->setParameterValue(index, value, false, false, false);
                break;
            }

            case kPluginBridgeNonRtSetProgram: {
                const int32_t index(fShmNonRtControl.readInt());

                if (plugin != nullptr && plugin->isEnabled())
                    plugin->setProgram(index, false, false, false);
                break;
            }

            case kPluginBridgeNonRtSetMidiProgram: {
                const int32_t index(fShmNonRtControl.readInt());

                if (plugin != nullptr && plugin->isEnabled())
                    plugin->setMidiProgram(index, false, false, false);
                break;
            }

            case kPluginBridgeNonRtSetCustomData: {
                // TODO
                break;
            }

            case kPluginBridgeNonRtSetChunkDataFile: {
                const uint32_t size(fShmNonRtControl.readUInt());
                CARLA_SAFE_ASSERT_BREAK(size > 0);

                char chunkFilePathTry[size+1];
                carla_zeroChar(chunkFilePathTry, size+1);
                fShmNonRtControl.readCustomData(chunkFilePathTry, size);

                CARLA_SAFE_ASSERT_BREAK(chunkFilePathTry[0] != '\0');
                if (plugin == nullptr || ! plugin->isEnabled()) break;

                String chunkFilePath(chunkFilePathTry);
#ifdef CARLA_OS_WIN
                if (chunkFilePath.startsWith("/"))
                {
                    // running under Wine, posix host
                    chunkFilePath = chunkFilePath.replaceSection(0, 1, "Z:\\");
                    chunkFilePath = chunkFilePath.replace("/", "\\");
                }
#endif

                File chunkFile(chunkFilePath);
                CARLA_SAFE_ASSERT_BREAK(chunkFile.existsAsFile());

                String chunkData(chunkFile.loadFileAsString());
                chunkFile.deleteFile();
                CARLA_SAFE_ASSERT_BREAK(chunkData.isNotEmpty());

                plugin->setChunkData(chunkData.toRawUTF8());
                break;
            }

            case kPluginBridgeNonRtPrepareForSave: {
                if (plugin == nullptr || ! plugin->isEnabled()) break;

                plugin->prepareForSave();

                for (uint32_t i=0, count=plugin->getCustomDataCount(); i<count; ++i)
                {
                    const CustomData& cdata(plugin->getCustomData(i));

                    oscSend_bridge_set_custom_data(cdata.type, cdata.key, cdata.value);
                }

                if (plugin->getOptionsEnabled() & PLUGIN_OPTION_USE_CHUNKS)
                {
                    if (const char* const chunkData = carla_get_chunk_data(0))
                    //if (const char* const chunkData = plugin->getChunkData())
                    {
                        String filePath(File::getSpecialLocation(File::tempDirectory).getFullPathName());

                        filePath += OS_SEP_STR;
                        filePath += ".CarlaChunk_";
                        filePath += fShmAudioPool.filename.buffer() + 18;

                        if (File(filePath).replaceWithText(chunkData))
                            oscSend_bridge_set_chunk_data_file(filePath.toRawUTF8());
                    }
                }

                oscSend_bridge_configure(CARLA_BRIDGE_MSG_SAVED, "");
                break;
            }

            case kPluginBridgeNonRtShowUI:
                if (plugin != nullptr && plugin->isEnabled())
                    plugin->showCustomUI(true);
                break;

            case kPluginBridgeNonRtHideUI:
                if (plugin != nullptr && plugin->isEnabled())
                    plugin->showCustomUI(false);
                break;

            case kPluginBridgeNonRtUiParameterChange: {
                // TODO
                break;
            }

            case kPluginBridgeNonRtUiProgramChange: {
                // TODO
                break;
            }

            case kPluginBridgeNonRtUiMidiProgramChange: {
                // TODO
                break;
            }

            case kPluginBridgeNonRtUiNoteOn: {
                // TODO
                break;
            }

            case kPluginBridgeNonRtUiNoteOff: {
                // TODO
                break;
            }

            case kPluginBridgeNonRtQuit:
                signalThreadShouldExit();
                break;
            }
        }
    }

    // -------------------------------------------------------------------

protected:
    void run() override
    {
        for (; ! shouldThreadExit();)
        {
            if (! jackbridge_sem_timedwait(&fShmRtControl.data->sem.server, 5))
            {
                if (errno == ETIMEDOUT)
                {
                    signalThreadShouldExit();
                    break;
                }
            }

            for (; fShmRtControl.isDataAvailableForReading();)
            {
                const PluginBridgeRtOpcode opcode(fShmRtControl.readOpcode());
                CarlaPlugin* const plugin(pData->plugins[0].plugin);

                if (opcode != kPluginBridgeRtProcess) {
                    carla_stdout("CarlaEngineBridgeRtThread::run() - got opcode: %s", PluginBridgeRtOpcode2str(opcode));
                }

                switch (opcode)
                {
                case kPluginBridgeRtNull:
                    break;

                case kPluginBridgeRtSetAudioPool: {
                    const uint64_t poolSize(fShmRtControl.readULong());
                    CARLA_SAFE_ASSERT_BREAK(poolSize > 0);
                    fShmAudioPool.data = (float*)jackbridge_shm_map(fShmAudioPool.shm, static_cast<size_t>(poolSize));
                    break;
                }

                case kPluginBridgeRtSetParameter: {
                    const uint32_t index(fShmRtControl.readUInt());
                    const float    value(fShmRtControl.readFloat());

                    if (plugin != nullptr && plugin->isEnabled())
                        plugin->setParameterValue(index, value, false, false, false);
                    break;
                }

                case kPluginBridgeRtSetProgram: {
                    const int32_t index(fShmRtControl.readInt());
                    CARLA_SAFE_ASSERT_BREAK(index >= -1);

                    if (plugin != nullptr && plugin->isEnabled())
                        plugin->setProgram(index, false, false, false);
                    break;
                }

                case kPluginBridgeRtSetMidiProgram: {
                    const int32_t index(fShmRtControl.readInt());
                    CARLA_SAFE_ASSERT_BREAK(index >= -1);

                    if (plugin != nullptr && plugin->isEnabled())
                        plugin->setMidiProgram(index, false, false, false);
                    break;
                }

                case kPluginBridgeRtMidiEvent: {
                    const uint32_t time(fShmRtControl.readUInt());
                    const uint32_t size(fShmRtControl.readUInt());
                    CARLA_SAFE_ASSERT_BREAK(size > 0 && size <= 4);

                    uint8_t data[size];

                    for (uint32_t i=0; i < size; ++i)
                        data[i] = fShmRtControl.readByte();

                    CARLA_SAFE_ASSERT_BREAK(pData->events.in != nullptr);

                    for (ushort i=0; i < kMaxEngineEventInternalCount; ++i)
                    {
                        EngineEvent& event(pData->events.in[i]);

                        if (event.type != kEngineEventTypeNull)
                            continue;

                        event.time = time;
                        event.fillFromMidiData(static_cast<uint8_t>(size), data);
                        break;
                    }
                    break;
                }

                case kPluginBridgeRtProcess: {
                    CARLA_SAFE_ASSERT_BREAK(fShmAudioPool.data != nullptr);

                    if (plugin != nullptr && plugin->isEnabled() && plugin->tryLock(true)) // FIXME - always lock?
                    {
                        const BridgeTimeInfo& bridgeTimeInfo(fShmRtControl.data->timeInfo);

                        const uint32_t inCount(plugin->getAudioInCount());
                        const uint32_t outCount(plugin->getAudioOutCount());

                        float* inBuffer[inCount];
                        float* outBuffer[outCount];

                        for (uint32_t i=0; i < inCount; ++i)
                            inBuffer[i] = fShmAudioPool.data + i*pData->bufferSize;
                        for (uint32_t i=0; i < outCount; ++i)
                            outBuffer[i] = fShmAudioPool.data + (i+inCount)*pData->bufferSize;

                        EngineTimeInfo& timeInfo(pData->timeInfo);

                        timeInfo.playing = bridgeTimeInfo.playing;
                        timeInfo.frame   = bridgeTimeInfo.frame;
                        timeInfo.usecs   = bridgeTimeInfo.usecs;
                        timeInfo.valid   = bridgeTimeInfo.valid;

                        if (timeInfo.valid & EngineTimeInfo::kValidBBT)
                        {
                            timeInfo.bbt.bar  = bridgeTimeInfo.bar;
                            timeInfo.bbt.beat = bridgeTimeInfo.beat;
                            timeInfo.bbt.tick = bridgeTimeInfo.tick;

                            timeInfo.bbt.beatsPerBar = bridgeTimeInfo.beatsPerBar;
                            timeInfo.bbt.beatType    = bridgeTimeInfo.beatType;

                            timeInfo.bbt.ticksPerBeat   = bridgeTimeInfo.ticksPerBeat;
                            timeInfo.bbt.beatsPerMinute = bridgeTimeInfo.beatsPerMinute;
                            timeInfo.bbt.barStartTick   = bridgeTimeInfo.barStartTick;
                        }

                        plugin->initBuffers();
                        plugin->process(inBuffer, outBuffer, pData->bufferSize);
                        plugin->unlock();
                    }

                    // clear buffer
                    CARLA_SAFE_ASSERT_BREAK(pData->events.in != nullptr);

                    if (pData->events.in[0].type != kEngineEventTypeNull)
                        carla_zeroStruct<EngineEvent>(pData->events.in, kMaxEngineEventInternalCount);

                    break;
                }
                }
            }

            if (! jackbridge_sem_post(&fShmRtControl.data->sem.client))
                carla_stderr2("Could not post to rt semaphore");
        }

        fIsRunning = false;
        callback(ENGINE_CALLBACK_ENGINE_STOPPED, 0, 0, 0, 0.0f, nullptr);
    }

    // -------------------------------------------------------------------

private:
    BridgeAudioPool    fShmAudioPool;
    BridgeRtControl    fShmRtControl;
    BridgeNonRtControl fShmNonRtControl;

    bool fIsRunning;
    bool fIsOffline;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineBridge)
};

// -----------------------------------------------------------------------

CarlaEngine* CarlaEngine::newBridge(const char* const audioBaseName, const char* const controlBaseName, const char* const timeBaseName)
{
    return new CarlaEngineBridge(audioBaseName, controlBaseName, timeBaseName);
}

CARLA_BACKEND_END_NAMESPACE

// -----------------------------------------------------------------------

#if defined(CARLA_OS_WIN) && ! defined(__WINE__)
extern "C" __declspec (dllexport)
#else
extern "C" __attribute__ ((visibility("default")))
#endif
void carla_register_native_plugin_carla();
void carla_register_native_plugin_carla(){}

// -----------------------------------------------------------------------
