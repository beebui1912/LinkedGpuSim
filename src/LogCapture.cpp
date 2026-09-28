/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "LogCapture.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>
#include <utility>
#include <vector>

#include <aclapi.h>
#include <sddl.h>

namespace SimApp
{

namespace
{

std::string NowStamp()
{
    using namespace std::chrono;
    const auto Now      = system_clock::now();
    const auto Sec      = time_point_cast<seconds>(Now);
    const auto Ms       = duration_cast<milliseconds>(Now - Sec).count();
    const std::time_t T = system_clock::to_time_t(Now);
    std::tm Local{};
#if defined(_MSC_VER)
    localtime_s(&Local, &T);
#else
    Local = *std::localtime(&T);
#endif
    char Buf[32];
    std::snprintf(Buf, sizeof(Buf), "%02d:%02d:%02d.%03lld",
                  Local.tm_hour, Local.tm_min, Local.tm_sec,
                  static_cast<long long>(Ms));
    return Buf;
}

std::string StripTrailingNewline(std::string Line)
{
    while (!Line.empty() && (Line.back() == '\r' || Line.back() == '\n'))
        Line.pop_back();
    return Line;
}

std::string BuildLogLine(const std::string& Prefix, const std::string& Line)
{
    std::string Cleaned = StripTrailingNewline(Line);
    std::ostringstream ss;
    ss << "[" << NowStamp() << "] ";
    if (!Prefix.empty())
        ss << "[" << Prefix << "] ";
    ss << Cleaned << "\n";
    return ss.str();
}

} // namespace


// -------------------------------- LogSink ---------------------------------

bool LogSink::Open(const std::filesystem::path& LogPath, bool EchoToConsole)
{
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if (m_File != nullptr)
    {
        std::fclose(m_File);
        m_File = nullptr;
    }
    m_EchoToConsole = EchoToConsole;
    m_LogPath       = LogPath;

    if (!LogPath.empty())
    {
        std::error_code Ec;
        if (LogPath.has_parent_path())
            std::filesystem::create_directories(LogPath.parent_path(), Ec);

#if defined(_MSC_VER)
        // Share read+write so users can `type` the log while it grows AND so
        // the injected shim (D3D12Sim.dll) can open the same file in append
        // mode from inside the child process.
        m_File = _wfsopen(LogPath.wstring().c_str(), L"wb", _SH_DENYNO);
#else
        m_File = std::fopen(LogPath.string().c_str(), "wb");
#endif
        if (m_File == nullptr)
            return false;

        // UTF-8 BOM so editors pick up the encoding immediately.
        const unsigned char Bom[3] = {0xEF, 0xBB, 0xBF};
        std::fwrite(Bom, 1, sizeof(Bom), m_File);
        std::fflush(m_File);
    }
    return true;
}

void LogSink::Close()
{
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if (m_File != nullptr)
    {
        std::fflush(m_File);
        std::fclose(m_File);
        m_File = nullptr;
    }
}

bool LogSink::IsOpen() const
{
    std::lock_guard<std::mutex> Lock(m_Mutex);
    return m_File != nullptr || m_EchoToConsole;
}

void LogSink::WriteToOutputs(const std::string& Text, bool ToStdout, bool ToStderr, bool ToFile)
{
    if (m_EchoToConsole && ToStderr)
    {
        std::fwrite(Text.data(), 1, Text.size(), stderr);
        std::fflush(stderr);
    }
    else if (m_EchoToConsole && ToStdout)
    {
        std::fwrite(Text.data(), 1, Text.size(), stdout);
        std::fflush(stdout);
    }
    if (ToFile && m_File != nullptr)
    {
        std::fwrite(Text.data(), 1, Text.size(), m_File);
        std::fflush(m_File);
    }
}

void LogSink::WriteLine(const std::string& Prefix, const std::string& Line)
{
    std::string Formatted = BuildLogLine(Prefix, Line);
    const bool  IsErr     = Prefix.find("err") != std::string::npos;
    std::lock_guard<std::mutex> Lock(m_Mutex);
    WriteToOutputs(Formatted, /*ToStdout*/ !IsErr, /*ToStderr*/ IsErr, /*ToFile*/ true);
}

void LogSink::WriteBlock(const std::string& Text)
{
    std::lock_guard<std::mutex> Lock(m_Mutex);
    WriteToOutputs(Text, /*ToStdout*/ true, /*ToStderr*/ false, /*ToFile*/ true);
}

void LogSink::WriteBanner(const std::string& Text)
{
    std::ostringstream ss;
    ss << "\n[" << NowStamp() << "] === " << Text << " ===\n";
    std::string Formatted = ss.str();
    std::lock_guard<std::mutex> Lock(m_Mutex);
    WriteToOutputs(Formatted, /*ToStdout*/ true, /*ToStderr*/ false, /*ToFile*/ true);
}

// -------------------------------- PipeReader ------------------------------

bool PipeReader::Start(HANDLE hPipeRead, std::string Prefix, LogSink& Sink, bool AsStderr)
{
    if (hPipeRead == nullptr || hPipeRead == INVALID_HANDLE_VALUE)
        return false;
    m_Stop.store(false, std::memory_order_release);
    m_Eof.store(false, std::memory_order_release);
    m_Thread = std::thread(&PipeReader::ReadLoop, this, hPipeRead, std::move(Prefix), &Sink, AsStderr);
    return true;
}

void PipeReader::Stop()
{
    m_Stop.store(true, std::memory_order_release);
    if (m_Thread.joinable())
        m_Thread.join();
}

void PipeReader::ReadLoop(HANDLE hPipeRead, std::string Prefix, LogSink* pSink, bool AsStderr)
{
    // Suffix the prefix so the sink can route to stderr for error streams.
    std::string EffectivePrefix = Prefix;
    if (AsStderr && EffectivePrefix.find("err") == std::string::npos)
        EffectivePrefix += ".err";

    constexpr DWORD kBufSize = 4096;
    std::vector<char> Buffer(kBufSize);
    std::string Line;
    Line.reserve(256);

    while (!m_Stop.load(std::memory_order_acquire))
    {
        DWORD BytesRead = 0;
        BOOL  Ok        = ::ReadFile(hPipeRead, Buffer.data(), kBufSize, &BytesRead, nullptr);
        if (!Ok || BytesRead == 0)
        {
            // Pipe closed or an error occurred.
            const DWORD Err = ::GetLastError();
            if (Err == ERROR_BROKEN_PIPE || BytesRead == 0)
                break;
            if (Err == ERROR_OPERATION_ABORTED)
                break;
            // Transient error: back off briefly and retry.
            ::Sleep(10);
            continue;
        }

        for (DWORD i = 0; i < BytesRead; ++i)
        {
            const char C = Buffer[i];
            if (C == '\n')
            {
                pSink->WriteLine(EffectivePrefix, Line);
                Line.clear();
            }
            else if (C == '\r')
            {
                // Skip carriage returns so Windows-style line endings collapse.
            }
            else
            {
                Line.push_back(C);
                // Guard against pathological unterminated output.
                if (Line.size() >= 65536)
                {
                    pSink->WriteLine(EffectivePrefix, Line);
                    Line.clear();
                }
            }
        }
    }

    if (!Line.empty())
        pSink->WriteLine(EffectivePrefix, Line);

    m_Eof.store(true, std::memory_order_release);
}

// ------------------------------ DebugOutputReader -------------------------

namespace
{

// Creates a security descriptor that grants full access to Everyone.  Needed
// so a low-integrity or a different-user child process can still signal the
// DBWIN events that we own.
class EveryoneSecurity
{
public:
    EveryoneSecurity()
    {
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:(A;;GA;;;WD)", SDDL_REVISION_1, &m_pSd, nullptr))
        {
            m_SA.nLength              = sizeof(m_SA);
            m_SA.lpSecurityDescriptor = m_pSd;
            m_SA.bInheritHandle       = FALSE;
            m_Valid                   = true;
        }
    }
    ~EveryoneSecurity()
    {
        if (m_pSd != nullptr)
            ::LocalFree(m_pSd);
    }
    SECURITY_ATTRIBUTES* Get() { return m_Valid ? &m_SA : nullptr; }

private:
    SECURITY_ATTRIBUTES  m_SA{};
    PSECURITY_DESCRIPTOR m_pSd  = nullptr;
    bool                 m_Valid = false;
};

HANDLE CreateOrOpenEventW(const wchar_t* Name, SECURITY_ATTRIBUTES* pSA, DWORD& OutLastError)
{
    HANDLE h = ::CreateEventW(pSA, FALSE, FALSE, Name);
    OutLastError = ::GetLastError();
    return h;
}

} // namespace

bool DebugOutputReader::Start(DWORD FilterPid, LogSink& Sink, std::string Prefix, std::string& OutError)
{
    OutError.clear();

    EveryoneSecurity Sec;
    SECURITY_ATTRIBUTES* pSA = Sec.Get();

    // Standard DBWIN object names used by OutputDebugString + DebugView.
    DWORD Err = 0;
    m_hBufferReady = CreateOrOpenEventW(L"DBWIN_BUFFER_READY", pSA, Err);
    if (m_hBufferReady == nullptr)
    {
        std::ostringstream ss;
        ss << "Failed to create/open DBWIN_BUFFER_READY event (Win32 error " << Err << "). "
           << "Another debug listener (e.g. DebugView) may already be running.";
        OutError = ss.str();
        CloseObjects();
        return false;
    }

    m_hDataReady = CreateOrOpenEventW(L"DBWIN_DATA_READY", pSA, Err);
    if (m_hDataReady == nullptr)
    {
        std::ostringstream ss;
        ss << "Failed to create/open DBWIN_DATA_READY event (Win32 error " << Err << ").";
        OutError = ss.str();
        CloseObjects();
        return false;
    }

    m_hMapping = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, pSA, PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
    if (m_hMapping == nullptr)
    {
        Err = ::GetLastError();
        std::ostringstream ss;
        ss << "Failed to create DBWIN_BUFFER file mapping (Win32 error " << Err << ").";
        OutError = ss.str();
        CloseObjects();
        return false;
    }

    m_pBuffer = ::MapViewOfFile(m_hMapping, FILE_MAP_READ, 0, 0, 4096);
    if (m_pBuffer == nullptr)
    {
        Err = ::GetLastError();
        std::ostringstream ss;
        ss << "Failed to map DBWIN_BUFFER (Win32 error " << Err << ").";
        OutError = ss.str();
        CloseObjects();
        return false;
    }

    m_hStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_hStopEvent == nullptr)
    {
        OutError = "Failed to create internal stop event.";
        CloseObjects();
        return false;
    }

    m_Stop.store(false, std::memory_order_release);
    m_Running.store(true, std::memory_order_release);

    // Kick things off: producers only write once BUFFER_READY is signalled.
    ::SetEvent(m_hBufferReady);

    m_Thread = std::thread(&DebugOutputReader::ReadLoop, this, FilterPid, &Sink, std::move(Prefix));
    return true;
}

void DebugOutputReader::Stop()
{
    if (!m_Running.load(std::memory_order_acquire) && !m_Thread.joinable())
        return;

    m_Stop.store(true, std::memory_order_release);
    if (m_hStopEvent != nullptr)
        ::SetEvent(m_hStopEvent);

    if (m_Thread.joinable())
        m_Thread.join();

    CloseObjects();
    m_Running.store(false, std::memory_order_release);
}

void DebugOutputReader::CloseObjects()
{
    if (m_pBuffer != nullptr)
    {
        ::UnmapViewOfFile(m_pBuffer);
        m_pBuffer = nullptr;
    }
    if (m_hMapping != nullptr)
    {
        ::CloseHandle(m_hMapping);
        m_hMapping = nullptr;
    }
    if (m_hBufferReady != nullptr)
    {
        ::CloseHandle(m_hBufferReady);
        m_hBufferReady = nullptr;
    }
    if (m_hDataReady != nullptr)
    {
        ::CloseHandle(m_hDataReady);
        m_hDataReady = nullptr;
    }
    if (m_hStopEvent != nullptr)
    {
        ::CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
    }
}

void DebugOutputReader::ReadLoop(DWORD FilterPid, LogSink* pSink, std::string Prefix)
{
    // Buffer layout:  { DWORD Pid; char Message[4096 - sizeof(DWORD)]; }
    const DWORD* pPid = reinterpret_cast<const DWORD*>(m_pBuffer);
    const char*  pMsg = reinterpret_cast<const char*>(m_pBuffer) + sizeof(DWORD);

    HANDLE WaitObjects[2] = {m_hDataReady, m_hStopEvent};

    while (!m_Stop.load(std::memory_order_acquire))
    {
        DWORD Result = ::WaitForMultipleObjects(2, WaitObjects, FALSE, INFINITE);
        if (Result == WAIT_OBJECT_0 + 1) // stop event
            break;
        if (Result != WAIT_OBJECT_0)
            break;

        const DWORD SrcPid = *pPid;
        // Copy out of the shared buffer immediately so the producer isn't blocked
        // any longer than necessary.
        std::string Msg;
        const size_t Cap = 4096 - sizeof(DWORD);
        Msg.reserve(64);
        for (size_t i = 0; i < Cap; ++i)
        {
            const char C = pMsg[i];
            if (C == '\0')
                break;
            Msg.push_back(C);
        }

        // Signal producer that the buffer is free again.
        ::SetEvent(m_hBufferReady);

        if (FilterPid != 0 && SrcPid != FilterPid)
            continue;

        // OutputDebugString messages often contain their own line breaks; split
        // them so the log stays one-line-per-entry.
        std::string Line;
        Line.reserve(Msg.size());
        for (char C : Msg)
        {
            if (C == '\n')
            {
                pSink->WriteLine(Prefix, Line);
                Line.clear();
            }
            else if (C != '\r')
            {
                Line.push_back(C);
            }
        }
        if (!Line.empty())
            pSink->WriteLine(Prefix, Line);
    }

    m_Running.store(false, std::memory_order_release);
}

} // namespace SimApp
