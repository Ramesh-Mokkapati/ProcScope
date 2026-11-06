// DependencyResolver.cpp
#include "DependencyResolver.h"
#include "DllExportReader.h"
#include "DllImportReader.h"
#include "FileVersionReader.h"
#include "LibraryAnalyzer.h"
#include "PeImage.h"
#include <algorithm>
#include <cwctype>

namespace
{
    std::wstring Lower(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        return text;
    }

    std::wstring TrimSlash(std::wstring path)
    {
        while (path.size() > 3 && path.back() == L'\\')
            path.pop_back();
        return path;
    }

    std::wstring Join(const std::wstring& folder, const std::wstring& name)
    {
        return !folder.empty() && folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L'\\');
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    bool FileExists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(ToExtendedPath(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    // The folder the loader uses as "System32" for a target of this bitness, whatever this build's is.
    std::wstring SystemFolderFor(bool target64)
    {
        wchar_t buffer[MAX_PATH] = {};
#ifdef _WIN64
        const UINT length = target64 ? GetSystemDirectoryW(buffer, MAX_PATH) : GetSystemWow64DirectoryW(buffer, MAX_PATH);
        return length ? std::wstring(buffer) : std::wstring();
#else
        if (!target64)
            return GetSystemDirectoryW(buffer, MAX_PATH) ? std::wstring(buffer) : std::wstring();
        // A 32-bit process reaches the real System32 through this alias.
        return GetWindowsDirectoryW(buffer, MAX_PATH) ? Join(buffer, L"Sysnative") : std::wstring();
#endif
    }

    // Compares the "6.0.26100.1_..." part of two WinSxS folder names, number by number.
    bool VersionedFolderIsNewer(const std::wstring& a, const std::wstring& b)
    {
        const auto version = [](const std::wstring& name) {
            std::vector<unsigned long> parts;
            const size_t start = name.find(L"_6.");
            if (start == std::wstring::npos)
                return parts;
            const wchar_t* p = name.c_str() + start + 1;
            for (;;)
            {
                wchar_t* end = nullptr;
                parts.push_back(wcstoul(p, &end, 10));
                if (*end != L'.')
                    break;
                p = end + 1;
            }
            return parts;
        };
        return version(a) > version(b);
    }

    // x64 code can load ARM64EC and ARM64X modules on ARM64 systems, and the reverse.
    bool MachinesCompatible(WORD root, WORD other)
    {
        if (root == other)
            return true;
        const auto x64Family = [](WORD m) { return m == IMAGE_FILE_MACHINE_AMD64 || m == 0xA641 || m == 0xA64E; };
        const auto armFamily = [](WORD m) { return m == IMAGE_FILE_MACHINE_ARM64 || m == 0xA641 || m == 0xA64E; };
        return (x64Family(root) && x64Family(other)) || (armFamily(root) && armFamily(other));
    }

    struct ModuleFacts
    {
        bool         valid = false;
        std::wstring error;
        WORD         machine = 0;
        bool         is64 = false;
        bool         anyCpu = false;   // .NET IL-only: 32-bit format, but runs as the host process's bitness
        WORD         subsystem = 0;
        std::wstring version;
        std::wstring company;
        std::vector<DllDependency> dependencies;
        // Value: index into forwards, or -1 for an export implemented in this module.
        std::unordered_map<std::string, int> byName;
        std::unordered_map<WORD, int>        byOrdinal;
        std::vector<std::string>             forwards;
        ULONGLONG    size = 0;
        ULONGLONG    modified = 0;
    };

    std::shared_ptr<const ModuleFacts> ReadFacts(const std::wstring& path)
    {
        auto facts = std::make_shared<ModuleFacts>();

        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (GetFileAttributesExW(ToExtendedPath(path).c_str(), GetFileExInfoStandard, &data))
        {
            facts->size = (ULONGLONG(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            facts->modified = (ULONGLONG(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        }

        PeImage image;
        if (!image.Open(path))
        {
            facts->error = image.Error();
            return facts;
        }
        facts->machine = image.GetHeaders().machine;
        facts->is64 = image.Is64();
        facts->subsystem = image.GetHeaders().subsystem;

        const IMAGE_DATA_DIRECTORY clr = image.Directory(IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR);
        if (clr.VirtualAddress && !image.Is64())
            RunGuarded([&] {
                if (const auto* header = image.At<IMAGE_COR20_HEADER>(clr.VirtualAddress))
                    facts->anyCpu = (header->Flags & COMIMAGE_FLAGS_ILONLY) && !(header->Flags & COMIMAGE_FLAGS_32BITREQUIRED);
            });

        DllExportReader exports;
        if (!exports.Load(image))
        {
            facts->error = exports.GetError();
            return facts;
        }
        DllImportReader imports;
        if (!imports.Load(image))
        {
            facts->error = imports.GetError();
            return facts;
        }

        facts->byName.reserve(exports.GetExports().size());
        for (const ExportedFunction& e : exports.GetExports())
        {
            int forward = -1;
            if (e.isForwarded)
            {
                forward = int(facts->forwards.size());
                facts->forwards.push_back(e.forwardName);
            }
            if (!e.name.empty())
                facts->byName[e.name] = forward;
            facts->byOrdinal[e.ordinal] = forward;
        }
        facts->dependencies = imports.GetDependencies();
        image.Close();

        VersionInfo version;
        if (FileVersionReader::Read(path, version))
        {
            facts->version = !version.fixedFileVersion.empty() ? version.fixedFileVersion : version.fileVersion;
            facts->company = version.companyName;
        }
        facts->valid = true;
        return facts;
    }
}

// ---------------------------------------------------------------------------
// ApiSetSchema

bool ApiSetSchema::LooksLikeApiSet(const std::wstring& lowerName)
{
    return lowerName.rfind(L"api-", 0) == 0 || lowerName.rfind(L"ext-", 0) == 0;
}

bool ApiSetSchema::Load(const std::wstring& schemaDllPath)
{
    m_entries.clear();
    m_loaded = false;

    PeImage image;
    if (!image.Open(schemaDllPath))
        return false;

    const BYTE* section = nullptr;
    size_t size = 0;
    if (!image.FindSection(".apiset", section, size))
        return false;

    const bool readable = RunGuarded([&] {
        const auto u32 = [&](size_t offset, DWORD& out) {
            if (offset > size || size - offset < sizeof(DWORD))
                return false;
            memcpy(&out, section + offset, sizeof(DWORD));
            return true;
        };
        const auto text = [&](DWORD offset, DWORD bytes, std::wstring& out) {
            if (offset > size || size - offset < bytes || bytes % 2)
                return false;
            out.assign(reinterpret_cast<const wchar_t*>(section + offset), bytes / 2);
            out = Lower(out);
            return true;
        };

        // API_SET_NAMESPACE, version 6 (Windows 10 and later).
        DWORD version = 0, count = 0, entriesOffset = 0;
        if (!u32(0, version) || version != 6 || !u32(12, count) || !u32(16, entriesOffset))
            return;
        const size_t kEntrySize = 24, kValueSize = 20;
        if (entriesOffset > size || count > (size - entriesOffset) / kEntrySize)
            return;

        for (DWORD i = 0; i < count; ++i)
        {
            const size_t e = size_t(entriesOffset) + i * kEntrySize;
            DWORD nameOffset = 0, nameLength = 0, hashedLength = 0, valueOffset = 0, valueCount = 0;
            if (!u32(e + 4, nameOffset) || !u32(e + 8, nameLength) || !u32(e + 12, hashedLength) ||
                !u32(e + 16, valueOffset) || !u32(e + 20, valueCount))
                return;
            if (hashedLength > nameLength || valueOffset > size || valueCount > (size - valueOffset) / kValueSize)
                continue;

            // The key leaves out the final version digit, which is how a newer name still matches.
            std::wstring key;
            if (!text(nameOffset, hashedLength, key))
                continue;

            Entry entry;
            for (DWORD v = 0; v < valueCount; ++v)
            {
                const size_t value = size_t(valueOffset) + v * kValueSize;
                DWORD importerOffset = 0, importerLength = 0, hostOffset = 0, hostLength = 0;
                if (!u32(value + 4, importerOffset) || !u32(value + 8, importerLength) ||
                    !u32(value + 12, hostOffset) || !u32(value + 16, hostLength))
                    break;
                std::wstring importer, host;
                if (!text(importerOffset, importerLength, importer) || !text(hostOffset, hostLength, host))
                    continue;
                if (importer.empty())
                    entry.defaultHost = host;
                else
                    entry.byImporter[importer] = host;
            }
            m_entries[key] = std::move(entry);
        }
        m_loaded = !m_entries.empty();
    });

    if (!readable)
    {
        m_entries.clear();
        m_loaded = false;
    }
    return m_loaded;
}

bool ApiSetSchema::Resolve(const std::wstring& lowerName, const std::wstring& lowerImporter, std::wstring& host) const
{
    std::wstring key = lowerName;
    const size_t dot = key.rfind(L".dll");
    if (dot != std::wstring::npos && dot + 4 == key.size())
        key.resize(dot);
    const size_t hyphen = key.find_last_of(L'-');
    if (hyphen == std::wstring::npos)
        return false;
    key.resize(hyphen);

    const auto it = m_entries.find(key);
    if (it == m_entries.end())
        return false;
    const auto alias = it->second.byImporter.find(lowerImporter);
    host = alias != it->second.byImporter.end() ? alias->second : it->second.defaultHost;
    return true;
}

// ---------------------------------------------------------------------------
// DependencyGraph

std::wstring DependencyGraph::SummaryText() const
{
    if (!failure.empty())
        return failure;
    const auto plural = [](size_t n, const wchar_t* one, const wchar_t* many) {
        return std::to_wstring(n) + L" " + (n == 1 ? one : many);
    };
    return plural(errors, L"error", L"errors") + L", " + plural(warnings, L"warning", L"warnings") +
           L" across " + plural(modules.size(), L"module", L"modules");
}

// ---------------------------------------------------------------------------
// DependencyResolver

struct DependencyResolver::Impl
{
    // Survive between Resolve calls.
    std::unordered_map<std::wstring, std::shared_ptr<const ModuleFacts>> cache;
    ApiSetSchema apiSets;
    bool         apiSetsTried = false;
    std::unordered_map<std::wstring, std::wstring> knownDlls;
    std::vector<std::wstring> pathFolders;
    mutable std::unordered_map<WORD, std::wstring> commonControls;   // by machine

    // One Resolve call.
    DependencyGraph*          graph = nullptr;
    WORD                      rootMachine = 0;
    std::wstring              appFolder, systemFolder, driversFolder, system16Folder, windowsFolder, nativeSystemFolder;
    std::unordered_map<std::wstring, size_t> moduleByName;
    std::vector<std::shared_ptr<const ModuleFacts>> factsOf;   // parallel to graph->modules
    std::deque<size_t>        pending;

    void LoadEnvironment()
    {
        if (apiSetsTried)
            return;
        apiSetsTried = true;

        wchar_t buffer[MAX_PATH] = {};
        if (GetSystemDirectoryW(buffer, MAX_PATH))
        {
            nativeSystemFolder = buffer;
            // A 32-bit process has System32 redirected, and the table may only exist in the real one.
            if (!apiSets.Load(Join(buffer, L"apisetschema.dll")))
                apiSets.Load(Join(SystemFolderFor(true), L"apisetschema.dll"));
        }

        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs", 0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            for (DWORD i = 0;; ++i)
            {
                wchar_t name[256], value[MAX_PATH];
                DWORD nameLength = _countof(name), valueBytes = sizeof(value), type = 0;
                const LSTATUS status = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(value), &valueBytes);
                if (status == ERROR_NO_MORE_ITEMS)
                    break;
                if (status != ERROR_SUCCESS || type != REG_SZ)
                    continue;
                value[_countof(value) - 1] = 0;
                const std::wstring file = Lower(value);
                if (file.size() > 4 && file.compare(file.size() - 4, 4, L".dll") == 0)
                    knownDlls[file] = value;
            }
            RegCloseKey(key);
        }

        // The loader's last resort. It's this process's PATH, the closest thing to what an app would inherit.
        const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (needed)
        {
            std::wstring path(needed, L'\0');
            const DWORD written = GetEnvironmentVariableW(L"PATH", &path[0], needed);
            path.resize(written);
            size_t start = 0;
            while (start <= path.size())
            {
                size_t end = path.find(L';', start);
                if (end == std::wstring::npos)
                    end = path.size();
                std::wstring folder = path.substr(start, end - start);
                folder.erase(std::remove(folder.begin(), folder.end(), L'"'), folder.end());
                if (!folder.empty())
                    pathFolders.push_back(TrimSlash(folder));
                start = end + 1;
            }
        }
    }

    std::shared_ptr<const ModuleFacts> Facts(const std::wstring& path)
    {
        const std::wstring key = Lower(path);
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        ULONGLONG size = 0, modified = 0;
        if (GetFileAttributesExW(ToExtendedPath(path).c_str(), GetFileExInfoStandard, &data))
        {
            size = (ULONGLONG(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            modified = (ULONGLONG(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        }
        const auto it = cache.find(key);
        if (it != cache.end() && it->second->size == size && it->second->modified == modified)
            return it->second;

        auto facts = ReadFacts(path);
        if (cache.size() > 4000)   // keeps a long session's memory bounded
            cache.clear();
        cache[key] = facts;
        return facts;
    }

    // System32 holds the old comctl32 v5, without functions such as TaskDialogIndirect. Almost every
    // program gets v6 through its manifest, so it's the fair copy to check imports against.
    // Other side-by-side assemblies aren't followed.
    std::wstring CommonControlsV6() const
    {
        const auto cached = commonControls.find(rootMachine);
        if (cached != commonControls.end())
            return cached->second;

        const wchar_t* arch = rootMachine == IMAGE_FILE_MACHINE_I386 ? L"x86"
                            : rootMachine == IMAGE_FILE_MACHINE_AMD64 ? L"amd64"
                            : rootMachine == IMAGE_FILE_MACHINE_ARM64 ? L"arm64" : nullptr;
        std::wstring best;
        if (arch)
        {
            const std::wstring sxs = Join(windowsFolder, L"WinSxS");
            const std::wstring pattern = Join(sxs, std::wstring(arch) + L"_microsoft.windows.common-controls_6595b64144ccf1df_6.*");
            std::wstring bestFolder;
            WIN32_FIND_DATAW fd;
            HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FileExists(Join(Join(sxs, fd.cFileName), L"comctl32.dll")) &&
                        (bestFolder.empty() || VersionedFolderIsNewer(fd.cFileName, bestFolder)))
                        bestFolder = fd.cFileName;
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
            if (!bestFolder.empty())
                best = Join(Join(sxs, bestFolder), L"comctl32.dll");
        }
        commonControls[rootMachine] = best;
        return best;
    }

    // The standard search order for an application at appFolder.
    // The current directory is left out, because this isn't the process that would be doing the loading.
    bool Locate(const std::wstring& lowerName, std::wstring& path, std::wstring& via) const
    {
        if (lowerName == L"comctl32.dll")
        {
            const std::wstring sideBySide = CommonControlsV6();
            if (!sideBySide.empty())
            {
                path = sideBySide;
                via = L"Side-by-side (v6)";
                return true;
            }
        }

        const auto known = knownDlls.find(lowerName);
        if (known != knownDlls.end() && FileExists(Join(systemFolder, known->second)))
        {
            path = Join(systemFolder, known->second);
            via = L"Known DLL";
            return true;
        }

        const auto tryFolder = [&](const std::wstring& folder, const wchar_t* label) {
            if (folder.empty())
                return false;
            const std::wstring candidate = Join(folder, lowerName);
            if (!FileExists(candidate))
                return false;
            path = candidate;
            via = label;
            return true;
        };

        if (tryFolder(appFolder, L"File's own folder") || tryFolder(systemFolder, L"System folder"))
            return true;
        // A .sys name is a driver whichever file asks for it: a user-mode file can reach the kernel in its chain.
        const bool isDriverName = lowerName.size() > 4 && lowerName.compare(lowerName.size() - 4, 4, L".sys") == 0;
        if (tryFolder(isDriverName && driversFolder.empty() ? Join(systemFolder, L"drivers") : driversFolder, L"Drivers folder"))
            return true;
        if (tryFolder(system16Folder, L"16-bit system folder") || tryFolder(windowsFolder, L"Windows folder"))
            return true;

        for (const std::wstring& entry : pathFolders)
        {
            // A 32-bit process that has System32 on its PATH actually reads SysWOW64.
            const bool isSystem = _wcsicmp(entry.c_str(), nativeSystemFolder.c_str()) == 0;
            if (tryFolder(isSystem ? systemFolder : entry, L"PATH"))
                return true;
        }
        return false;
    }

    size_t AddModule(const std::wstring& lowerName, const std::wstring& displayName, const std::wstring& path,
                     const std::wstring& via, DepStatus status, const std::wstring& detail)
    {
        DepModule module;
        module.name = displayName;
        module.path = path;
        module.foundVia = via;
        module.status = status;
        module.detail = detail;

        std::shared_ptr<const ModuleFacts> facts;
        if (status == DepStatus::Ok)
        {
            facts = Facts(path);
            if (!facts->valid)
            {
                module.status = DepStatus::Invalid;
                module.detail = facts->error;
            }
            else
            {
                module.machine = facts->machine;
                module.version = facts->version;
                module.company = facts->company;
                if (!graph->modules.empty() && !facts->anyCpu && !MachinesCompatible(rootMachine, facts->machine))
                {
                    module.status = DepStatus::WrongArchitecture;
                    module.detail = L"This is a " + MachineName(facts->machine) + L" file, but the chain needs " + MachineName(rootMachine);
                }
            }
        }

        const size_t id = graph->modules.size();
        graph->modules.push_back(std::move(module));
        factsOf.push_back(facts);
        moduleByName[lowerName] = id;
        if (graph->modules[id].status == DepStatus::Ok)
            pending.push_back(id);
        return id;
    }

    // Finds or creates the module an import of `importedAs` by `importer` refers to.
    size_t ResolveName(const std::string& importedAs, size_t importer, bool& viaApiSet)
    {
        std::wstring lowerName = Lower(ToWide(importedAs));
        if (lowerName.find(L'.') == std::wstring::npos)
            lowerName += L".dll";

        viaApiSet = false;
        if (ApiSetSchema::LooksLikeApiSet(lowerName) && apiSets.IsLoaded())
        {
            std::wstring host;
            if (apiSets.Resolve(lowerName, Lower(graph->modules[importer].name), host))
            {
                viaApiSet = true;
                if (host.empty())
                {
                    const auto it = moduleByName.find(lowerName);
                    if (it != moduleByName.end())
                        return it->second;
                    return AddModule(lowerName, ToWide(importedAs), std::wstring(), std::wstring(), DepStatus::ApiSetNoHost,
                                     L"This API set has no implementation on this system");
                }
                lowerName = host;
            }
        }

        const auto it = moduleByName.find(lowerName);
        if (it != moduleByName.end())
            return it->second;

        std::wstring path, via;
        if (!Locate(lowerName, path, via))
        {
            // Extension API sets are optional by design, and this system's table may not list them all.
            if (lowerName.rfind(L"ext-", 0) == 0)
                return AddModule(lowerName, ToWide(importedAs), std::wstring(), std::wstring(), DepStatus::ApiSetNoHost,
                                 L"This API set has no implementation on this system");
            return AddModule(lowerName, ToWide(importedAs), std::wstring(), std::wstring(), DepStatus::NotFound,
                             L"Not found in the search path");
        }
        return AddModule(lowerName, FileNameOf(path), path, via, DepStatus::Ok, std::wstring());
    }

    // A forward names its module without an extension. In user mode that means .dll; in a kernel-mode
    // chain it can be ntoskrnl.exe or a .sys driver as well.
    std::string ForwardModuleName(const std::string& stem, size_t module) const
    {
        const bool kernelModule = factsOf[module] && factsOf[module]->subsystem == IMAGE_SUBSYSTEM_NATIVE;
        if (!driversFolder.empty() || kernelModule)
        {
            for (const char* extension : { ".sys", ".exe" })
            {
                const std::string candidate = stem + extension;
                const std::wstring lower = Lower(ToWide(candidate));
                std::wstring path, via;
                if (moduleByName.count(lower) || Locate(lower, path, via))
                    return candidate;
            }
        }
        return stem + ".dll";
    }

    // Records that `module` forwards a function to `target`. False if the text isn't a usable forward.
    bool Forward(size_t module, const std::string& target, size_t& edgeIndex, size_t& functionIndex)
    {
        // "DLL.Function" or "DLL.#ordinal"
        const size_t dot = target.find_last_of('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= target.size())
            return false;
        const std::string dll = ForwardModuleName(target.substr(0, dot), module);
        const std::string function = target.substr(dot + 1);

        bool viaApiSet = false;
        const size_t targetModule = ResolveName(dll, module, viaApiSet);

        std::deque<DepEdge>& edges = graph->modules[module].edges;
        edgeIndex = edges.size();
        for (size_t i = 0; i < edges.size(); ++i)
            if (edges[i].kind == DepKind::Forward && edges[i].target == targetModule)
            {
                edgeIndex = i;
                break;
            }
        if (edgeIndex == edges.size())
        {
            DepEdge created;
            created.kind = DepKind::Forward;
            created.target = targetModule;
            created.importedAs = dll;
            created.viaApiSet = viaApiSet;
            edges.push_back(std::move(created));
        }

        DepFunction request;
        if (function[0] == '#')
        {
            request.byOrdinal = true;
            request.ordinal = WORD(strtoul(function.c_str() + 1, nullptr, 10));
        }
        else
            request.name = function;
        functionIndex = Request(edges[edgeIndex], std::move(request));
        return true;
    }

    // Checks one function against the edge's target. Returns its index in edge.functions.
    size_t Request(DepEdge& edge, DepFunction request)
    {
        const std::string key = request.byOrdinal ? "#" + std::to_string(request.ordinal) : request.name;
        const auto existing = edge.functionIndex.find(key);
        if (existing != edge.functionIndex.end())
            return existing->second;

        const size_t targetId = edge.target;
        const std::shared_ptr<const ModuleFacts>& facts = factsOf[targetId];
        std::string forwardTo;

        if (graph->modules[targetId].status != DepStatus::Ok || !facts)
        {
            request.checked = false;
        }
        else
        {
            int forward = -1;
            bool found = false;
            if (request.byOrdinal)
            {
                const auto it = facts->byOrdinal.find(request.ordinal);
                if (it != facts->byOrdinal.end())
                {
                    found = true;
                    forward = it->second;
                }
            }
            else
            {
                const auto it = facts->byName.find(request.name);
                if (it != facts->byName.end())
                {
                    found = true;
                    forward = it->second;
                }
            }
            request.found = found;
            if (!found)
                ++edge.missingFunctions;
            else if (forward >= 0)
            {
                forwardTo = facts->forwards[forward];
                request.forwardedTo = forwardTo;
            }
        }

        const size_t index = edge.functions.size();
        edge.functionIndex[key] = index;
        edge.functions.push_back(std::move(request));

        // Modules and edges live in deques, so `edge` stays valid while this adds more.
        // A forward loop ends at the lookup above, because the function is already recorded.
        if (!forwardTo.empty())
        {
            size_t forwardEdge = 0, forwardFunction = 0;
            if (Forward(targetId, forwardTo, forwardEdge, forwardFunction))
            {
                edge.functions[index].forwardEdge = forwardEdge;
                edge.functions[index].forwardFunction = forwardFunction;
            }
        }
        return index;
    }

    void ProcessModule(size_t id)
    {
        const std::shared_ptr<const ModuleFacts> facts = factsOf[id];
        for (const DllDependency& dependency : facts->dependencies)
        {
            bool viaApiSet = false;
            const size_t target = ResolveName(dependency.dllName, id, viaApiSet);

            DepEdge edge;
            edge.target = target;
            edge.kind = dependency.isDelayLoaded ? DepKind::Delay : DepKind::Static;
            edge.importedAs = dependency.dllName;
            edge.viaApiSet = viaApiSet;
            graph->modules[id].edges.push_back(std::move(edge));
            DepEdge& stored = graph->modules[id].edges.back();

            for (const ImportedFunction& f : dependency.functions)
            {
                DepFunction request;
                request.name = f.name;
                request.ordinal = f.ordinal;
                request.byOrdinal = f.isOrdinal;
                Request(stored, std::move(request));
            }
        }
    }

    std::deque<size_t> requiredQueue;

    void MarkEdgeRequired(DepEdge& edge)
    {
        edge.required = true;
        DepModule& target = graph->modules[edge.target];
        if (!target.required)
        {
            target.required = true;
            requiredQueue.push_back(edge.target);
        }
    }

    // A forward only forces its target to load when something required really imports that function.
    void MarkFunctionRequired(DepEdge& edge, size_t functionIndex)
    {
        DepFunction& function = edge.functions[functionIndex];
        if (function.required)
            return;
        function.required = true;
        if (function.forwardEdge == size_t(-1))
            return;
        DepEdge& next = graph->modules[edge.target].edges[function.forwardEdge];
        MarkEdgeRequired(next);
        MarkFunctionRequired(next, function.forwardFunction);
    }

    void MarkRequired()
    {
        requiredQueue.clear();
        graph->modules[0].required = true;
        requiredQueue.push_back(0);
        while (!requiredQueue.empty())
        {
            const size_t id = requiredQueue.front();
            requiredQueue.pop_front();
            for (DepEdge& edge : graph->modules[id].edges)
            {
                if (edge.kind != DepKind::Static)
                    continue;
                MarkEdgeRequired(edge);
                for (size_t f = 0; f < edge.functions.size(); ++f)
                    MarkFunctionRequired(edge, f);
            }
        }
    }

    void CollectProblems()
    {
        for (size_t m = 0; m < graph->modules.size(); ++m)
        {
            const DepModule& importer = graph->modules[m];
            for (size_t e = 0; e < importer.edges.size(); ++e)
            {
                const DepEdge& edge = importer.edges[e];
                const DepModule& target = graph->modules[edge.target];
                const wchar_t* how = edge.kind == DepKind::Forward ? L"forwards to" : edge.kind == DepKind::Delay ? L"delay-loads" : L"needs";
                const std::wstring asWritten = ToWide(edge.importedAs);

                DepProblem problem;
                problem.importer = m;
                problem.edge = e;
                problem.severity = edge.required ? DepSeverity::Error : DepSeverity::Warning;

                switch (target.status)
                {
                case DepStatus::NotFound:
                    problem.text = importer.name + L" " + how + L" " + asWritten + L", which was not found.";
                    break;
                case DepStatus::WrongArchitecture:
                    problem.text = importer.name + L" " + how + L" " + asWritten + L": " + target.detail + L" (" + target.path + L").";
                    break;
                case DepStatus::Invalid:
                    problem.text = importer.name + L" " + how + L" " + asWritten + L", which can't be read: " + target.detail + L" (" + target.path + L").";
                    break;
                case DepStatus::ApiSetNoHost:
                    problem.text = importer.name + L" " + how + L" " + asWritten + L", an API set with no implementation on this system.";
                    // Extension API sets are optional by design: ntoskrnl.exe itself imports ones
                    // with no host, and still runs. So this never counts as a certain failure.
                    problem.severity = edge.required ? DepSeverity::Warning : DepSeverity::Info;
                    break;
                default:
                    if (!edge.missingFunctions)
                        continue;
                    {
                        problem.text = importer.name + L" " + how + L" " + std::to_wstring(edge.missingFunctions) +
                                       (edge.missingFunctions == 1 ? L" function" : L" functions") + L" that " + target.name + L" doesn't export: ";
                        size_t shown = 0;
                        for (const DepFunction& f : edge.functions)
                        {
                            if (f.found)
                                continue;
                            if (shown == 5)
                            {
                                problem.text += L", ...";
                                break;
                            }
                            if (shown++)
                                problem.text += L", ";
                            problem.text += f.byOrdinal ? L"#" + std::to_wstring(f.ordinal) : ToWide(f.name);
                        }
                    }
                    break;
                }
                graph->problems.push_back(std::move(problem));
            }
        }

        std::stable_sort(graph->problems.begin(), graph->problems.end(),
                         [](const DepProblem& a, const DepProblem& b) { return int(a.severity) < int(b.severity); });
        for (const DepProblem& p : graph->problems)
        {
            graph->errors += p.severity == DepSeverity::Error ? 1 : 0;
            graph->warnings += p.severity == DepSeverity::Warning ? 1 : 0;
        }
    }
};

DependencyResolver::DependencyResolver() : m_impl(std::make_unique<Impl>()) {}
DependencyResolver::~DependencyResolver() = default;

std::shared_ptr<DependencyGraph> DependencyResolver::Resolve(const std::wstring& rootPath, const std::function<bool()>& cancelled)
{
    auto graph = std::make_shared<DependencyGraph>();
    Impl& impl = *m_impl;
    impl.LoadEnvironment();

    impl.graph = graph.get();
    impl.moduleByName.clear();
    impl.factsOf.clear();
    impl.pending.clear();

    const auto rootFacts = impl.Facts(rootPath);
    if (!rootFacts->valid)
    {
        graph->failure = rootFacts->error;
        impl.graph = nullptr;
        return graph;
    }

    impl.rootMachine = rootFacts->machine;
    bool rootIs64 = rootFacts->is64;
    if (rootFacts->anyCpu)
    {
        // Any-CPU code takes the bitness of whatever hosts it; assume this system's native one.
        SYSTEM_INFO info = {};
        GetNativeSystemInfo(&info);
        rootIs64 = info.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL;
        impl.rootMachine = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? WORD(IMAGE_FILE_MACHINE_ARM64)
                         : rootIs64 ? WORD(IMAGE_FILE_MACHINE_AMD64) : WORD(IMAGE_FILE_MACHINE_I386);
    }
    const size_t slash = rootPath.find_last_of(L'\\');
    impl.appFolder = slash == std::wstring::npos ? std::wstring() : TrimSlash(rootPath.substr(0, slash + 1));
    impl.systemFolder = SystemFolderFor(rootIs64);
    // A 32-bit file stored in the real System32 is seen through redirection, as SysWOW64, by the process running it.
    if (_wcsicmp(impl.appFolder.c_str(), SystemFolderFor(true).c_str()) == 0)
        impl.appFolder = impl.systemFolder;
    // Kernel-mode and boot-time files link against the kernel and other drivers.
    const WORD subsystem = rootFacts->subsystem;
    const bool kernelMode = subsystem == IMAGE_SUBSYSTEM_NATIVE || subsystem == IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION ||
                            subsystem == IMAGE_SUBSYSTEM_EFI_APPLICATION || subsystem == IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER ||
                            subsystem == IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER;
    impl.driversFolder = kernelMode ? Join(impl.systemFolder, L"drivers") : std::wstring();

    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    impl.windowsFolder = windows;
    impl.system16Folder = Join(windows, L"System");

    // The root takes part under its own name, so a dependency importing it back joins up with it.
    impl.AddModule(Lower(FileNameOf(rootPath)), FileNameOf(rootPath), rootPath, L"Selected file", DepStatus::Ok, std::wstring());

    while (!impl.pending.empty())
    {
        if (cancelled && cancelled())
        {
            graph->cancelled = true;
            break;
        }
        const size_t id = impl.pending.front();
        impl.pending.pop_front();
        impl.ProcessModule(id);
    }

    if (!graph->cancelled)
    {
        impl.MarkRequired();
        impl.CollectProblems();
    }
    for (DepModule& module : graph->modules)
        for (DepEdge& edge : module.edges)
            edge.functionIndex.clear();   // only needed while building

    impl.factsOf.clear();
    impl.graph = nullptr;
    return graph;
}
