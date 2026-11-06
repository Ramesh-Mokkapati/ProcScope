// ProcessSampler.h : what's running. One live table of processes, plus the modules and threads of a process.
// No MFC, so Tests\ReaderTests can check it on its own.
#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>

struct ProcessRow
{
    DWORD        pid = 0;
    DWORD        parentPid = 0;
    std::wstring name;
    ULONGLONG    createTime = 0;      // FILETIME; with pid, identifies this process even if the pid is reused

    // Live values.
    double       cpuPercent = 0.0;    // share of the whole machine
    ULONGLONG    cpuTime = 0;         // 100 ns units
    ULONGLONG    workingSet = 0;
    ULONGLONG    privateBytes = 0;
    double       readPerSec = 0.0;
    double       writePerSec = 0.0;
    DWORD        threads = 0;
    DWORD        handles = 0;
    DWORD        priorityClass = 0;
    bool         accessDenied = false;

    // Fixed for the life of the process.
    std::wstring path;
    std::wstring commandLine;
    std::wstring architecture;
    std::wstring user;
    std::wstring version;
    std::wstring company;
    std::wstring description;
    DWORD        session = 0;
    bool         elevated = false;
};

struct ProcessSnapshot
{
    std::vector<ProcessRow> rows;
    double    cpuPercent = 0.0;
    ULONGLONG memoryUsed = 0;
    ULONGLONG memoryTotal = 0;
    ULONGLONG threads = 0;
    ULONGLONG handles = 0;
};

// CPU and I/O rates come from the difference against the previous Sample call.
class ProcessSampler
{
public:
    void Sample(ProcessSnapshot& out);

private:
    struct History
    {
        ULONGLONG createTime = 0;
        ULONGLONG cpuTime = 0;
        ULONGLONG readBytes = 0;
        ULONGLONG writeBytes = 0;
    };
    struct Fixed
    {
        std::wstring path, commandLine, architecture, user, version, company, description;
        DWORD        session = 0;
        bool         elevated = false;
    };

    const Fixed& FixedInfo(DWORD pid, ULONGLONG createTime, HANDLE process);
    std::wstring AccountName(HANDLE process, bool& elevated);

    std::unordered_map<DWORD, History>      m_history;
    std::unordered_map<ULONGLONG, Fixed>    m_fixed;     // key: pid and createTime combined
    std::unordered_map<std::wstring, std::wstring> m_accounts;   // SID text to account name
    std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> m_versions;   // path to version, company
    ULONGLONG m_lastSystem = 0;
    ULONGLONG m_lastIdle = 0;
    LARGE_INTEGER m_lastTime = {};
};

struct LoadedModule
{
    std::wstring name;
    std::wstring path;
    ULONGLONG    base = 0;
    DWORD        size = 0;
    std::wstring version;
    std::wstring company;
};

struct ThreadRow
{
    DWORD        id = 0;
    LONG         priority = 0;
    ULONGLONG    startAddress = 0;
    std::wstring startText;          // "module.dll+0x1234"
    ULONGLONG    cpuTime = 0;        // 100 ns units; 0 if it couldn't be read
    ULONGLONG    created = 0;        // FILETIME
};

// False with an explanation when the process can't be inspected.
bool EnumerateModules(DWORD pid, std::vector<LoadedModule>& out, std::wstring& error);
void EnumerateThreads(DWORD pid, const std::vector<LoadedModule>& modules, std::vector<ThreadRow>& out);

std::wstring PriorityClassName(DWORD priorityClass);
