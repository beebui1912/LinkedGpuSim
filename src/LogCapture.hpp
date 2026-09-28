/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

//  LogCapture
//  ----------
//  Three pieces that cooperate to capture "everything the child says":
//
//   * LogSink            - thread-safe writer that tees to the parent's stdout
//                          and (optionally) a UTF-8 log file, one line at a
//                          time, with a wall-clock timestamp and per-source
//                          prefix.  All other pieces write through this.
//
//   * PipeReader         - reader thread that reads bytes from a child stdout
//                          or stderr pipe (see ProcessLauncher), splits them
//                          into lines and forwards each line to a LogSink.
//
//   * DebugOutputReader  - DebugView-style consumer that opens the DBWIN
//                          shared-memory buffer used by OutputDebugString,
//                          filters by the child's PID and forwards each
//                          message to a LogSink.

#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace SimApp
{

class LogSink
{
public:
    LogSink()  = default;
    ~LogSink() { Close(); }

    LogSink(const LogSink&)            = delete;
    LogSink& operator=(const LogSink&) = delete;

    // Opens the file (truncating), enables console echo. LogPath may be empty
    // to disable file output.
    bool Open(const std::filesystem::path& LogPath, bool EchoToConsole = true);
    void Close();
    bool IsOpen() const;

    // Writes a one-line entry: "[timestamp] [prefix] message".  The message
    // must not contain trailing newlines.
    void WriteLine(const std::string& Prefix, const std::string& Line);

    // Writes a raw block of text (multi-line ok, no timestamp/prefix).
    // Used for the info panel / banner blocks.
    void WriteBlock(const std::string& Text);

    // Timestamped banner ("=== ... ===") with a bit of vertical space.
    void WriteBanner(const std::string& Text);

    const std::filesystem::path& GetLogPath() const { return m_LogPath; }

private:
    void WriteToOutputs(const std::string& Text, bool ToStdout, bool ToStderr, bool ToFile);

    mutable std::mutex    m_Mutex;
    std::FILE*            m_File          = nullptr;
    bool                  m_EchoToConsole = true;
    std::filesystem::path m_LogPath;
};

class PipeReader
{
public:
    PipeReader() = default;
    ~PipeReader() { Stop(); }

    PipeReader(const PipeReader&)            = delete;
    PipeReader& operator=(const PipeReader&) = delete;

    // Starts the reader thread. Prefix is included in every emitted line
    // (typical values: "child.out" and "child.err"). Ownership of hPipeRead
    // stays with the caller; PipeReader stops reading if the pipe is closed.
    bool Start(HANDLE hPipeRead, std::string Prefix, LogSink& Sink, bool AsStderr = false);

    // Signals the thread to stop and joins.  Safe to call repeatedly.
    void Stop();

    // True once the reader thread has observed EOF on the pipe.
    bool IsEof() const { return m_Eof.load(std::memory_order_acquire); }

private:
    void ReadLoop(HANDLE hPipeRead, std::string Prefix, LogSink* pSink, bool AsStderr);

    std::thread       m_Thread;
    std::atomic<bool> m_Stop{false};
    std::atomic<bool> m_Eof{false};
};

class DebugOutputReader
{
public:
    DebugOutputReader() = default;
    ~DebugOutputReader() { Stop(); }

    DebugOutputReader(const DebugOutputReader&)            = delete;
    DebugOutputReader& operator=(const DebugOutputReader&) = delete;

    // Starts the DBWIN consumer.  Only lines whose source PID equals FilterPid
    // are forwarded; pass 0 to forward every OutputDebugString on the machine.
    // Returns false with the reason in OutError if the DBWIN objects cannot be
    // acquired (e.g. another debugger holds them).
    bool Start(DWORD FilterPid, LogSink& Sink, std::string Prefix, std::string& OutError);
    void Stop();

    bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }

private:
    void ReadLoop(DWORD FilterPid, LogSink* pSink, std::string Prefix);
    void CloseObjects();

    std::thread       m_Thread;
    std::atomic<bool> m_Stop{false};
    std::atomic<bool> m_Running{false};

    HANDLE m_hMapping     = nullptr;
    void*  m_pBuffer      = nullptr;
    HANDLE m_hBufferReady = nullptr; // Event: parent signals "buffer free".
    HANDLE m_hDataReady   = nullptr; // Event: producer signals "data ready".
    HANDLE m_hStopEvent   = nullptr; // Local event used to unblock the wait.
};

} // namespace SimApp
