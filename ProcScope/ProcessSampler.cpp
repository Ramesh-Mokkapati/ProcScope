// ProcessSampler.cpp
#include "ProcessSampler.h"
#include "FileVersionReader.h"
#include "LibraryAnalyzer.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <sddl.h>
#include <algorithm>
#include <cwctype>
#include <iterator>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    ULONGLONG ToU64(const FILETIME& ft)
    {
        return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    std::wstring Lower(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        return text;
    }

    using NtQueryInformationProcessFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    FARPROC NtFunction(const char* name)
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? GetProcAddress(ntdll, name) : nullptr;
    }

    std::wstring ReadCommandLine(HANDLE process)
    {
        // ProcessCommandLineInformation needs only limited query access, unlike reading the PEB.
        static const auto query = reinterpret_cast<NtQueryInformationProcessFn>(NtFunction("NtQueryInformationProcess"));
        if (!query)
            return std::wstring();
        const ULONG kProcessCommandLineInformation = 60;
        std::vector<BYTE> buffer(8192);
        ULONG needed = 0;
        LONG status = query(process, kProcessCommandLineInformation, buffer.data(), ULONG(buffer.size()), &needed);
        if (status < 0 && needed > buffer.size() && needed < 1024 * 1024)
        {
            buffer.resize(needed);
            status = query(process, kProcessCommandLineInformation, buffer.data(), ULONG(buffer.size()), &needed);
        }
        if (status < 0)
            return std::wstring();

        struct CountedText
        {
            USHORT Length;
            USHORT MaximumLength;
            PWSTR  Buffer;
        };
        const auto* text = reinterpret_cast<const CountedText*>(buffer.data());
        return text->Buffer ? std::wstring(text->Buffer, text->Length / sizeof(wchar_t)) : std::wstring();
    }

    std::wstring ProcessArchitecture(HANDLE process)
    {
        USHORT processMachine = 0, nativeMachine = 0;
        if (!IsWow64Process2(process, &processMachine, &nativeMachine))
            return std::wstring();
        return MachineName(processMachine != IMAGE_FILE_MACHINE_UNKNOWN ? processMachine : nativeMachine);
    }
}

std::wstring ProcessSampler::AccountName(HANDLE process, bool& elevated)
{
    elevated = false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return std::wstring();

    std::wstring account;
    TOKEN_ELEVATION elevation = {};
    DWORD length = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length))
        elevated = elevation.TokenIsElevated != 0;

    BYTE userBuffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
    if (GetTokenInformation(token, TokenUser, userBuffer, sizeof(userBuffer), &length))
    {
        const PSID sid = reinterpret_cast<TOKEN_USER*>(userBuffer)->User.Sid;
        wchar_t* sidText = nullptr;
        if (ConvertSidToStringSidW(sid, &sidText))
        {
            const std::wstring key = sidText;
            LocalFree(sidText);
            // A lookup can take a while, and there are only a few accounts.
            const auto known = m_accounts.find(key);
            if (known != m_accounts.end())
                account = known->second;
            else
            {
                wchar_t name[256], domain[256];
                DWORD nameLength = _countof(name), domainLength = _countof(domain);
                SID_NAME_USE use;
                if (LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use))
                    account = domain[0] ? std::wstring(domain) + L"\\" + name : std::wstring(name);
                m_accounts[key] = account;
            }
        }
    }
    CloseHandle(token);
    return account;
}

const ProcessSampler::Fixed& ProcessSampler::FixedInfo(DWORD pid, ULONGLONG createTime, HANDLE process)
{
    const ULONGLONG key = (createTime << 16) ^ pid;
    const auto existing = m_fixed.find(key);
    if (existing != m_fixed.end())
        return existing->second;

    Fixed info;
    wchar_t path[32768];
    DWORD length = _countof(path);
    if (QueryFullProcessImageNameW(process, 0, path, &length))
        info.path = path;
    info.commandLine = ReadCommandLine(process);
    info.architecture = ProcessArchitecture(process);
    info.user = AccountName(process, info.elevated);
    ProcessIdToSessionId(pid, &info.session);

    if (!info.path.empty())
    {
        const std::wstring versionKey = Lower(info.path);
        const auto known = m_versions.find(versionKey);
        if (known != m_versions.end())
        {
            info.version = known->second.first;
            info.company = known->second.second;
        }
        else
        {
            VersionInfo version;
            if (FileVersionReader::Read(info.path, version))
            {
                info.version = !version.fixedFileVersion.empty() ? version.fixedFileVersion : version.fileVersion;
                info.company = version.companyName;
                info.description = version.fileDescription;
            }
            m_versions[versionKey] = { info.version, info.company };
        }
    }
    return m_fixed.emplace(key, std::move(info)).first->second;
}

void ProcessSampler::Sample(ProcessSnapshot& out)
{
    out = ProcessSnapshot();

    FILETIME idleTime, kernelTime, userTime;
    GetSystemTimes(&idleTime, &kernelTime, &userTime);
    const ULONGLONG system = ToU64(kernelTime) + ToU64(userTime);   // kernel time includes idle time
    const ULONGLONG idle = ToU64(idleTime);
    const ULONGLONG systemDelta = m_lastSystem && system > m_lastSystem ? system - m_lastSystem : 0;
    const ULONGLONG idleDelta = idle >= m_lastIdle ? idle - m_lastIdle : 0;

    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double elapsed = m_lastTime.QuadPart ? double(now.QuadPart - m_lastTime.QuadPart) / double(frequency.QuadPart) : 0.0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    std::unordered_map<DWORD, History> history;
    history.reserve(m_history.size() + 32);
    std::unordered_map<ULONGLONG, bool> stillRunning;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
    {
        ProcessRow row;
        row.pid = entry.th32ProcessID;
        row.parentPid = entry.th32ParentProcessID;
        row.name = row.pid == 0 ? L"System Idle Process" : entry.szExeFile;
        row.threads = entry.cntThreads;
        out.threads += entry.cntThreads;

        HANDLE process = row.pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid) : nullptr;
        if (process)
        {
            History h;
            FILETIME created, exited, kernel, user;
            if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            {
                h.createTime = ToU64(created);
                h.cpuTime = ToU64(kernel) + ToU64(user);
            }
            row.createTime = h.createTime;
            row.cpuTime = h.cpuTime;

            PROCESS_MEMORY_COUNTERS_EX memory = {};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            {
                row.workingSet = memory.WorkingSetSize;
                row.privateBytes = memory.PrivateUsage;
            }

            GetProcessHandleCount(process, &row.handles);
            out.handles += row.handles;
            row.priorityClass = GetPriorityClass(process);

            IO_COUNTERS io = {};
            if (GetProcessIoCounters(process, &io))
            {
                h.readBytes = io.ReadTransferCount;
                h.writeBytes = io.WriteTransferCount;
            }

            const Fixed& fixed = FixedInfo(row.pid, row.createTime, process);
            row.path = fixed.path;
            row.commandLine = fixed.commandLine;
            row.architecture = fixed.architecture;
            row.user = fixed.user;
            row.version = fixed.version;
            row.company = fixed.company;
            row.description = fixed.description;
            row.session = fixed.session;
            row.elevated = fixed.elevated;
            stillRunning[(row.createTime << 16) ^ row.pid] = true;

            CloseHandle(process);

            // A rate only means something when the pid still refers to the same process.
            const auto previous = m_history.find(row.pid);
            if (previous != m_history.end() && previous->second.createTime == h.createTime)
            {
                if (systemDelta && h.cpuTime >= previous->second.cpuTime)
                    row.cpuPercent = min(100.0, 100.0 * double(h.cpuTime - previous->second.cpuTime) / double(systemDelta));
                if (elapsed > 0.0)
                {
                    if (h.readBytes >= previous->second.readBytes)
                        row.readPerSec = double(h.readBytes - previous->second.readBytes) / elapsed;
                    if (h.writeBytes >= previous->second.writeBytes)
                        row.writePerSec = double(h.writeBytes - previous->second.writeBytes) / elapsed;
                }
            }
            history[row.pid] = h;
        }
        else if (row.pid != 0)
        {
            row.accessDenied = true;
        }
        out.rows.push_back(std::move(row));
    }
    CloseHandle(snapshot);

    if (systemDelta)
    {
        out.cpuPercent = min(100.0, max(0.0, 100.0 * double(systemDelta - min(idleDelta, systemDelta)) / double(systemDelta)));
        // The idle process can't be opened, but its share is known.
        for (ProcessRow& row : out.rows)
            if (row.pid == 0)
                row.cpuPercent = 100.0 - out.cpuPercent;
    }

    // Exited processes shouldn't accumulate over a long session.
    for (auto it = m_fixed.begin(); it != m_fixed.end();)
        it = stillRunning.count(it->first) ? std::next(it) : m_fixed.erase(it);

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        out.memoryTotal = memory.ullTotalPhys;
        out.memoryUsed = memory.ullTotalPhys - memory.ullAvailPhys;
    }

    m_history.swap(history);
    m_lastSystem = system;
    m_lastIdle = idle;
    m_lastTime = now;
}

bool EnumerateModules(DWORD pid, std::vector<LoadedModule>& out, std::wstring& error)
{
    out.clear();
    error.clear();

    HANDLE snapshot = INVALID_HANDLE_VALUE;
    // ERROR_BAD_LENGTH means the module list changed while the snapshot was being taken.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            break;
    }
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED)
            error = L"Access denied: run ProcScope as administrator to see this process's loaded modules.";
        else if (code == ERROR_PARTIAL_COPY)
            error = L"A 32-bit build can't read a 64-bit process: use the x64 build.";
        else
            error = L"The loaded modules could not be read (Windows error " + std::to_wstring(code) + L").";
        return false;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
    {
        LoadedModule module;
        module.name = entry.szModule;
        module.path = entry.szExePath;
        module.base = reinterpret_cast<ULONG_PTR>(entry.modBaseAddr);
        module.size = entry.modBaseSize;
        VersionInfo version;
        if (FileVersionReader::Read(module.path, version))
        {
            module.version = !version.fixedFileVersion.empty() ? version.fixedFileVersion : version.fileVersion;
            module.company = version.companyName;
        }
        out.push_back(std::move(module));
    }
    CloseHandle(snapshot);
    return true;
}

void EnumerateThreads(DWORD pid, const std::vector<LoadedModule>& modules, std::vector<ThreadRow>& out)
{
    out.clear();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(NtFunction("NtQueryInformationThread"));

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid)
            continue;

        ThreadRow row;
        row.id = entry.th32ThreadID;
        row.priority = entry.tpBasePri;

        if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, row.id))
        {
            FILETIME created, exited, kernel, user;
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
            {
                row.created = ToU64(created);
                row.cpuTime = ToU64(kernel) + ToU64(user);
            }
            CloseHandle(thread);
        }

        // The start address needs more access than the times do.
        if (HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, row.id))
        {
            const ULONG kThreadQuerySetWin32StartAddress = 9;
            PVOID start = nullptr;
            if (query && query(thread, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) >= 0)
            {
                row.startAddress = reinterpret_cast<ULONG_PTR>(start);
                for (const LoadedModule& module : modules)
                    if (row.startAddress >= module.base && row.startAddress - module.base < module.size)
                    {
                        wchar_t offset[32];
                        swprintf_s(offset, L"+0x%llX", row.startAddress - module.base);
                        row.startText = module.name + offset;
                        break;
                    }
            }
            CloseHandle(thread);
        }
        out.push_back(std::move(row));
    }
    CloseHandle(snapshot);
}

std::wstring PriorityClassName(DWORD priorityClass)
{
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:         return L"Low";
    case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
    case NORMAL_PRIORITY_CLASS:       return L"Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
    case HIGH_PRIORITY_CLASS:         return L"High";
    case REALTIME_PRIORITY_CLASS:     return L"Realtime";
    default:                          return std::wstring();
    }
}
