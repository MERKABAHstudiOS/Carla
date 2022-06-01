/*
 * Carla Log Thread
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

#ifndef CARLA_LOG_THREAD_HPP_INCLUDED
#define CARLA_LOG_THREAD_HPP_INCLUDED

#include "CarlaBackend.h"
#include "CarlaString.hpp"
#include "CarlaThread.hpp"

#include <fcntl.h>

#ifdef CARLA_OS_WIN
# include <io.h>
# define _close(fd) close(fd)
# define _dup2(f1,f2) dup2(f1,f2)
#endif

using CARLA_BACKEND_NAMESPACE::EngineCallbackFunc;

// -----------------------------------------------------------------------
// Log thread

class CarlaLogThread : private CarlaThread
{
public:
    CarlaLogThread()
        : CarlaThread("CarlaLogThread"),
          fStdOut(-1),
          fStdErr(-1),
          fCallback(nullptr),
          fCallbackPtr(nullptr) {}

    ~CarlaLogThread()
    {
        stop();
    }

    void init()
    {
        std::fflush(stdout);
        std::fflush(stderr);

#ifdef CARLA_OS_WIN
        // TODO: use process id instead
        const int randint = std::rand();

        char strBuf[0xff+1];
        strBuf[0xff] = '\0';
        std::snprintf(strBuf, 0xff, "\\\\.\\pipe\\carlalogthread-%i", randint);

        fPipe[0] = CreateNamedPipeA(strBuf, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT, 2, 4096, 4096, 0, nullptr);
        fPipe[1] = CreateFileA(strBuf, GENERIC_WRITE, 0x0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        CARLA_SAFE_ASSERT_RETURN(fPipe[0] != INVALID_HANDLE_VALUE,);
        CARLA_SAFE_ASSERT_RETURN(fPipe[1] != INVALID_HANDLE_VALUE,);

        const int pipe1 = _open_osfhandle((INT_PTR)fPipe[1], _O_WRONLY | _O_BINARY);
#else
        CARLA_SAFE_ASSERT_RETURN(pipe(fPipe) == 0,);

        if (fcntl(fPipe[0], F_SETFL, O_NONBLOCK) != 0)
        {
            close(fPipe[0]);
            close(fPipe[1]);
            return;
        }

        const int pipe1 = fPipe[1];
#endif

        fStdOut = dup(STDOUT_FILENO);
        fStdErr = dup(STDERR_FILENO);

        dup2(pipe1, STDOUT_FILENO);
        dup2(pipe1, STDERR_FILENO);

        startThread();
    }

    void stop()
    {
        if (fStdOut == -1)
            return;

        stopThread(5000);

        std::fflush(stdout);
        std::fflush(stderr);

#ifdef CARLA_OS_WIN
        CloseHandle(fPipe[0]);
        CloseHandle(fPipe[1]);
#else
        close(fPipe[0]);
        close(fPipe[1]);
#endif

        dup2(fStdOut, STDOUT_FILENO);
        dup2(fStdErr, STDERR_FILENO);
        close(fStdOut);
        close(fStdErr);
        fStdOut = -1;
        fStdErr = -1;
    }

    void setCallback(EngineCallbackFunc callback, void* callbackPtr)
    {
        CARLA_SAFE_ASSERT_RETURN(callback != nullptr,);

        fCallback    = callback;
        fCallbackPtr = callbackPtr;
    }

protected:
    void run()
    {
        CARLA_SAFE_ASSERT_RETURN(fCallback != nullptr,);

        size_t k, bufTempPos;
        ssize_t r, lastRead;
        char bufTemp[1024+1];
        char bufRead[1024+1];
        char bufSend[2048+1];

        bufTemp[0] = '\0';
        bufTempPos = 0;

        while (! shouldThreadExit())
        {
            bufRead[0] = '\0';

            while ((r = read(fPipe[0], bufRead, 1024)) > 0)
            {
                CARLA_SAFE_ASSERT_CONTINUE(r <= 1024);

                bufRead[r] = '\0';
                lastRead = 0;

                for (ssize_t i=0; i<r; ++i)
                {
                    CARLA_SAFE_ASSERT_BREAK(bufRead[i] != '\0');

                    if (bufRead[i] != '\n')
                        continue;

                    k = static_cast<size_t>(i-lastRead);

                    if (bufTempPos != 0)
                    {
                        std::memcpy(bufSend, bufTemp, bufTempPos);
                        std::memcpy(bufSend+bufTempPos, bufRead+lastRead, k);
                        k += bufTempPos;
                    }
                    else
                    {
                        std::memcpy(bufSend, bufRead+lastRead, k);
                    }

                    lastRead   = i+1;
                    bufSend[k] = '\0';
                    bufTemp[0] = '\0';
                    bufTempPos = 0;

                    fCallback(fCallbackPtr, CARLA_BACKEND_NAMESPACE::ENGINE_CALLBACK_DEBUG, 0, 0, 0, 0, 0.0f, bufSend);
                }

                if (lastRead > 0 && lastRead != r)
                {
                    k = static_cast<size_t>(r-lastRead);
                    std::memcpy(bufTemp, bufRead+lastRead, k);
                    bufTemp[k] = '\0';
                    bufTempPos = k;
                }
            }

            carla_msleep(20);
        }
    }

private:
#ifdef CARLA_OS_WIN
    HANDLE fPipe[2];
#else
    int fPipe[2];
#endif

    int fStdOut;
    int fStdErr;

    EngineCallbackFunc fCallback;
    void*              fCallbackPtr;

#ifdef CARLA_OS_WIN
    ssize_t read(const HANDLE pipeh, void* const buf, DWORD numBytes)
    {
        if (ReadFile(pipeh, buf, numBytes, &numBytes, nullptr) != FALSE)
            return numBytes;
        return -1;
    }
#endif

    //CARLA_PREVENT_HEAP_ALLOCATION
    CARLA_DECLARE_NON_COPYABLE(CarlaLogThread)
};

#ifdef CARLA_OS_WIN
# undef close
# undef dup2
#endif

// -----------------------------------------------------------------------

#endif // CARLA_LOG_THREAD_HPP_INCLUDED
