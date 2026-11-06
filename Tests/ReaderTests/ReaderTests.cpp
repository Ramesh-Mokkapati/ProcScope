// ReaderTests.cpp : console harness for the reader classes (no GUI, no MFC).
//
//   ReaderTests dump <file>              Canonical listing, compared with dumpbin by Tests\CompareWithDumpbin.ps1
//   ReaderTests info <file>...           Human-readable summary, including the signature
//   ReaderTests fuzz <file> <iterations> Runs every reader on corrupted copies of <file>; any crash fails the run
//   ReaderTests deps <file>              Dependency chain: modules, problems, and a final "errors= warnings=" line
//   ReaderTests sweep <folder>           Problem counts for each library directly in a folder
//   ReaderTests loadercheck [pid]        Compares the worked-out chain with the DLLs a running process really has loaded
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "DependencyResolver.h"
#include "FileSignatureVerifier.h"
#include "LibArchiveReader.h"
#include "LibraryAnalyzer.h"
#include "ProcessSampler.h"

#pragma comment(lib, "psapi.lib")

namespace
{
    const wchar_t* SeverityName(DepSeverity s)
    {
        return s == DepSeverity::Error ? L"ERROR" : s == DepSeverity::Warning ? L"WARNING" : L"INFO";
    }

    int Deps(const std::wstring& path, const wchar_t* whyModule)
    {
        DependencyResolver resolver;
        const auto graph = resolver.Resolve(path);
        if (!graph->failure.empty())
        {
            wprintf(L"FAILURE %s\n", graph->failure.c_str());
            return 2;
        }
        if (whyModule)
        {
            // Which links lead to one module, for tracing a surprising result.
            for (const DepModule& m : graph->modules)
                for (const DepEdge& e : m.edges)
                    if (_wcsicmp(graph->modules[e.target].name.c_str(), whyModule) == 0)
                    {
                        wprintf(L"WHY %s[%s] kind=%d required=%d as=%S\n", m.name.c_str(), m.required ? L"req" : L"opt", int(e.kind), e.required, e.importedAs.c_str());
                        for (size_t i = 0; i < e.functions.size() && i < 4; ++i)
                            wprintf(L"     %S required=%d\n", e.functions[i].name.c_str(), e.functions[i].required);
                    }
            return 0;
        }
        for (const DepModule& m : graph->modules)
            wprintf(L"MODULE %s|%d|%s|%s|%s\n", m.name.c_str(), int(m.status), m.required ? L"required" : L"optional", m.foundVia.c_str(), m.path.c_str());
        for (const DepProblem& p : graph->problems)
            wprintf(L"%s %s\n", SeverityName(p.severity), p.text.c_str());
        wprintf(L"errors=%zu warnings=%zu modules=%zu\n", graph->errors, graph->warnings, graph->modules.size());
        return 0;
    }

    int Sweep(const std::wstring& folder)
    {
        DependencyResolver resolver;
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
            return 2;
        size_t files = 0, clean = 0, withErrors = 0;
        const DWORD start = GetTickCount();
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsLibraryFileName(fd.cFileName, false))
                continue;
            const auto graph = resolver.Resolve(folder + L"\\" + fd.cFileName);
            if (!graph->failure.empty())
                continue;
            ++files;
            clean += graph->errors == 0 ? 1 : 0;
            withErrors += graph->errors ? 1 : 0;
            for (const DepProblem& p : graph->problems)
                if (p.severity == DepSeverity::Error)
                    wprintf(L"%s: %s\n", fd.cFileName, p.text.c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        wprintf(L"swept %zu files in %.1f s: %zu with no errors, %zu with errors\n", files, (GetTickCount() - start) / 1000.0, clean, withErrors);
        return 0;
    }

    std::wstring LowerText(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        return s;
    }

    // A process that runs proves two things: every module the chain calls required has to be loaded
    // from exactly that path, and there can't be any errors.
    int LoaderCheck(DWORD pid)
    {
        HANDLE process = pid ? OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid) : GetCurrentProcess();
        if (!process)
        {
            wprintf(L"cannot open process %u\n", pid);
            return 2;
        }
        wchar_t image[32768] = {};
        DWORD imageLength = _countof(image);
        QueryFullProcessImageNameW(process, 0, image, &imageLength);

        HMODULE modules[4096];
        DWORD needed = 0;
        if (!EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        {
            wprintf(L"cannot list modules of %s\n", image);
            return 2;
        }
        // A WOW64 process reports some of its modules under System32 and others under SysWOW64.
        const auto NormalizeLoaded = [](std::wstring path) {
            path = LowerText(path);
            const size_t at = path.find(L"\\system32\\");
#ifndef _WIN64
            if (at != std::wstring::npos)
                path.replace(at, 10, L"\\syswow64\\");
#else
            (void)at;
#endif
            return path;
        };
        std::set<std::wstring> loaded;
        // Loaded from WinSxS through a manifest: a known limit of working out the chain statically.
        std::set<std::wstring> sideBySide;
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < _countof(modules); ++i)
        {
            wchar_t name[32768] = {};
            if (GetModuleFileNameExW(process, modules[i], name, _countof(name)))
            {
                loaded.insert(NormalizeLoaded(name));
                const std::wstring lower = LowerText(name);
                if (lower.find(L"\\winsxs\\") != std::wstring::npos)
                    sideBySide.insert(lower.substr(lower.find_last_of(L'\\') + 1));
            }
        }

        DependencyResolver resolver;
        const auto graph = resolver.Resolve(image);
        size_t required = 0, matched = 0, redirected = 0, different = 0;
        for (const DepModule& m : graph->modules)
        {
            if (!m.required || m.path.empty())
                continue;
            ++required;
            if (loaded.count(NormalizeLoaded(m.path)))
                ++matched;
            else if (sideBySide.count(LowerText(m.name)))
                ++redirected;
            else
            {
                ++different;
                wprintf(L"  not loaded from this path: %s (%s)\n", m.path.c_str(), m.foundVia.c_str());
            }
        }
        for (const DepProblem& p : graph->problems)
            if (p.severity == DepSeverity::Error)
                wprintf(L"  false error: %s\n", p.text.c_str());
        wprintf(L"%s: %zu loaded; required %zu, matched %zu, side-by-side %zu, wrong %zu; errors %zu; modules in chain %zu\n",
                image, loaded.size(), required, matched, redirected, different, graph->errors, graph->modules.size());
        return different || graph->errors ? 3 : 0;
    }

    int Dump(const std::wstring& path)
    {
        LibraryDetails d;
        LoadDetails(path, d);
        if (!d.entry.error.empty())
        {
            wprintf(L"ERROR %s\n", d.entry.error.c_str());
            return 2;
        }

        std::vector<std::string> lines;
        for (const auto& e : d.exports)
        {
            char line[8192];
            if (d.isArchive)
                sprintf_s(line, "S %s", e.name.c_str());
            else
                sprintf_s(line, "E %u %s %08X %s", e.ordinal, e.name.empty() ? "[NONAME]" : e.name.c_str(), e.isForwarded ? 0 : e.rva, e.forwardName.c_str());
            lines.push_back(line);
        }
        if (d.isPe)
        {
            for (const auto& dep : d.dependencies)
            {
                std::string dll = dep.dllName;
                std::transform(dll.begin(), dll.end(), dll.begin(), [](char c) { return char(tolower(static_cast<unsigned char>(c))); });
                for (const auto& f : dep.functions)
                {
                    char line[8192];
                    if (f.isOrdinal)
                        sprintf_s(line, "%c %s #%u", dep.isDelayLoaded ? 'D' : 'I', dll.c_str(), f.ordinal);
                    else
                        sprintf_s(line, "%c %s %s", dep.isDelayLoaded ? 'D' : 'I', dll.c_str(), f.name.c_str());
                    lines.push_back(line);
                }
            }
        }
        std::sort(lines.begin(), lines.end());
        for (const auto& l : lines)
            printf("%s\n", l.c_str());
        return 0;
    }

    int Info(const std::wstring& path)
    {
        LibraryEntry quick;
        FillEntryFromPath(path, quick);
        AnalyzeQuick(quick);

        LibraryDetails d;
        LoadDetails(path, d);

        size_t functions = 0;
        for (const auto& dep : d.dependencies)
            functions += dep.functions.size();

        SignatureInfo sig;
        FileSignatureVerifier::Verify(path, sig);

        wprintf(L"%s\n", path.c_str());
        wprintf(L"  type=%s arch=%s exports=%zu (quick %lld) deps=%zu (quick %lld) funcs=%zu\n",
                d.entry.type.c_str(), d.entry.architecture.c_str(), d.exports.size(), quick.exports,
                d.dependencies.size(), quick.dependencies, functions);
        wprintf(L"  version=%s company=%s error=%s\n", d.entry.version.c_str(), d.entry.company.c_str(), d.entry.error.c_str());
        wprintf(L"  signature valid=%d catalog=%d revocationChecked=%d status=0x%08X signer=%s\n    %s\n",
                sig.valid, sig.catalogSigned, sig.revocationChecked, static_cast<unsigned>(sig.status), sig.signer.c_str(), sig.message.c_str());

        // The list row and the details tabs have to agree.
        const bool consistent = !d.entry.error.empty() || quick.exports == (long long)d.exports.size();
        return consistent ? 0 : 3;
    }

    int Fuzz(const std::wstring& path, int iterations)
    {
        HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (in == INVALID_HANDLE_VALUE)
            return 2;
        DWORD size = GetFileSize(in, nullptr), read = 0;
        std::vector<BYTE> original(size);
        ReadFile(in, original.data(), size, &read, nullptr);
        CloseHandle(in);

        wchar_t tempDir[MAX_PATH], tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        GetTempFileNameW(tempDir, L"lsf", 0, tempFile);

        std::mt19937 random(0x1B5C09E);
        const size_t headerZone = original.size() < 4096 ? original.size() : 4096;
        size_t parsed = 0;
        DependencyResolver resolver;   // shared, so its cache isn't rebuilt for every corrupted copy

        for (int i = 0; i < iterations; ++i)
        {
            std::vector<BYTE> data = original;
            const int mode = i % 5;
            const int edits = 1 + int(random() % 40);
            for (int e = 0; e < edits; ++e)
            {
                // Mostly the headers, where corruption drives the parser the furthest.
                const size_t zone = mode == 3 ? data.size() : headerZone;
                const size_t at = random() % zone;
                switch (mode)
                {
                case 0: data[at] = BYTE(random()); break;
                case 1: data[at] = 0xFF; break;
                case 2: data[at] = 0x00; break;
                default:
                    if (at + 4 <= data.size())
                        *reinterpret_cast<DWORD*>(&data[at]) = random();
                    break;
                }
            }
            if (mode == 4)
                data.resize(1 + random() % data.size());

            HANDLE out = CreateFileW(tempFile, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD written = 0;
            WriteFile(out, data.data(), DWORD(data.size()), &written, nullptr);
            CloseHandle(out);

            LibraryEntry quick;
            FillEntryFromPath(tempFile, quick);
            AnalyzeQuick(quick);

            LibraryDetails details;
            LoadDetails(tempFile, details);
            if (details.entry.error.empty())
                ++parsed;

            LibArchiveReader archive;
            archive.Load(tempFile);

            resolver.Resolve(tempFile);

            ApiSetSchema schema;
            std::wstring host;
            if (schema.Load(tempFile))
                schema.Resolve(L"api-ms-win-core-file-l1-1-0.dll", L"kernel32.dll", host);
        }
        DeleteFileW(tempFile);
        wprintf(L"fuzz ok: %d iterations, %zu still parsed, %s\n", iterations, parsed, path.c_str());
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3 && wcscmp(argv[1], L"dump") == 0)
        return Dump(argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"info") == 0)
    {
        int worst = 0;
        for (int i = 2; i < argc; ++i)
        {
            const int result = Info(argv[i]);
            if (result > worst)
                worst = result;
        }
        return worst;
    }
    if (argc >= 4 && wcscmp(argv[1], L"fuzz") == 0)
        return Fuzz(argv[2], _wtoi(argv[3]));
    if (argc >= 2 && wcscmp(argv[1], L"procs") == 0)
    {
        // Two samples, so the CPU and I/O rates have something to compare against.
        ProcessSampler sampler;
        ProcessSnapshot snapshot;
        sampler.Sample(snapshot);
        Sleep(500);
        sampler.Sample(snapshot);
        for (const ProcessRow& r : snapshot.rows)
            wprintf(L"%u|%u|%s|%.1f|%llu|%u|%u|%d|%s\n", r.pid, r.parentPid, r.name.c_str(), r.cpuPercent, r.workingSet, r.threads, r.handles, r.accessDenied, r.architecture.c_str());
        wprintf(L"total cpu=%.1f memoryUsed=%llu memoryTotal=%llu threads=%llu handles=%llu\n", snapshot.cpuPercent, snapshot.memoryUsed, snapshot.memoryTotal, snapshot.threads, snapshot.handles);
        return 0;
    }
    if (argc >= 3 && wcscmp(argv[1], L"modules") == 0)
    {
        const DWORD pid = DWORD(_wtoi(argv[2]));
        std::vector<LoadedModule> modules;
        std::wstring error;
        if (!EnumerateModules(pid, modules, error))
        {
            wprintf(L"ERROR %s\n", error.c_str());
            return 2;
        }
        for (const LoadedModule& m : modules)
            wprintf(L"M %s\n", m.path.c_str());
        std::vector<ThreadRow> threads;
        EnumerateThreads(pid, modules, threads);
        for (const ThreadRow& t : threads)
            wprintf(L"T %u %s\n", t.id, t.startText.c_str());
        return 0;
    }
    if (argc >= 3 && wcscmp(argv[1], L"deps") == 0)
        return Deps(argv[2], argc >= 4 ? argv[3] : nullptr);
    if (argc >= 3 && wcscmp(argv[1], L"sweep") == 0)
        return Sweep(argv[2]);
    if (argc >= 2 && wcscmp(argv[1], L"loadercheck") == 0)
        return LoaderCheck(argc >= 3 ? DWORD(_wtoi(argv[2])) : 0);
    fwprintf(stderr, L"usage: ReaderTests dump <file> | info <file>... | fuzz <file> <iterations> | deps <file> | sweep <folder> | loadercheck [pid]\n");
    return 1;
}
