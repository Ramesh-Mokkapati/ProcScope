// LibraryAnalyzer.cpp
#include "LibraryAnalyzer.h"
#include "LibArchiveReader.h"
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <map>

namespace
{
    struct ExtensionType
    {
        const wchar_t* extension;
        const wchar_t* type;
    };

    const ExtensionType kLibraryTypes[] = {
        { L"dll",   L"DLL" },
        { L"ocx",   L"ActiveX Control" },
        { L"cpl",   L"Control Panel Applet" },
        { L"drv",   L"Driver Library" },
        { L"sys",   L"Kernel Driver" },
        { L"ax",    L"DirectShow Filter" },
        { L"acm",   L"Audio Codec" },
        { L"ime",   L"Input Method" },
        { L"tsp",   L"Telephony Provider" },
        { L"rll",   L"Resource Library" },
        { L"pyd",   L"Python Extension" },
        { L"node",  L"Node.js Addon" },
        { L"xll",   L"Excel Add-in" },
        { L"winmd", L"Windows Metadata" },
        { L"lib",   L"Library" },
        { L"a",     L"Library" },
    };

    std::wstring Extension(const std::wstring& fileName)
    {
        const size_t dot = fileName.find_last_of(L'.');
        if (dot == std::wstring::npos)
            return std::wstring();
        std::wstring ext = fileName.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        return ext;
    }

    std::wstring PeType(const std::wstring& fileName, const PeImage& image, bool managed)
    {
        const std::wstring ext = Extension(fileName);
        std::wstring type;
        if (!(image.GetHeaders().characteristics & IMAGE_FILE_DLL) && ext == L"exe")
            type = L"Executable";
        else
        {
            type = L"DLL";
            for (const ExtensionType& t : kLibraryTypes)
                if (ext == t.extension)
                    type = t.type;
        }
        if (managed)
            type += L" (.NET)";
        return type;
    }

    bool IsManaged(const PeImage& image)
    {
        return image.Directory(IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR).VirtualAddress != 0;
    }

    std::wstring Bitness(const PeImage::Headers& h)
    {
        return MachineName(h.machine);
    }
}

std::wstring LibraryEntry::FullPath() const
{
    if (folder.empty())
        return name;
    return folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
}

bool IsLibraryFileName(const std::wstring& fileName, bool includeExecutables)
{
    const std::wstring ext = Extension(fileName);
    if (includeExecutables && ext == L"exe")
        return true;
    for (const ExtensionType& t : kLibraryTypes)
        if (ext == t.extension)
            return true;
    return false;
}

std::wstring LibraryExtensionList()
{
    std::wstring list;
    for (const ExtensionType& t : kLibraryTypes)
    {
        if (!list.empty())
            list += L", ";
        list += L".";
        list += t.extension;
    }
    return list;
}

bool FillEntryFromPath(const std::wstring& fullPath, LibraryEntry& entry)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(ToExtendedPath(fullPath).c_str(), GetFileExInfoStandard, &data))
        return false;
    const size_t slash = fullPath.find_last_of(L'\\');
    entry.folder = slash == std::wstring::npos ? std::wstring() : fullPath.substr(0, slash);
    entry.name = slash == std::wstring::npos ? fullPath : fullPath.substr(slash + 1);
    if (entry.folder.size() == 2 && entry.folder[1] == L':')
        entry.folder += L'\\';
    entry.size = (ULONGLONG(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    entry.modified = (ULONGLONG(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return true;
}

void AnalyzeQuick(LibraryEntry& entry)
{
    const std::wstring path = entry.FullPath();
    const std::wstring ext = Extension(entry.name);

    if (ext == L"lib" || ext == L"a")
    {
        if (LibArchiveReader::IsArchive(path))
        {
            LibArchiveReader archive;
            if (!archive.Load(path))
            {
                entry.type = L"Library";
                entry.error = archive.GetError();
                return;
            }
            entry.type = archive.IsImportLibrary() ? L"Import Library" : L"Static Library";
            entry.architecture = MachineName(archive.GetMachine());
            entry.exports = (long long)archive.GetSymbols().size();
            entry.dependencies = (long long)archive.GetMembers().size();
            return;
        }
    }

    PeImage image;
    if (!image.Open(path))
    {
        entry.type = L"Unrecognised";
        entry.error = image.Error();
        return;
    }

    entry.type = PeType(entry.name, image, IsManaged(image));
    entry.architecture = Bitness(image.GetHeaders());
    entry.exports = DllExportReader::Count(image);
    entry.dependencies = DllImportReader::CountDependencies(image);
    image.Close();   // GetFileVersionInfo opens the file itself

    VersionInfo version;
    if (FileVersionReader::Read(path, version))
    {
        entry.version = !version.fixedFileVersion.empty() ? version.fixedFileVersion : version.fileVersion;
        entry.company = version.companyName;
    }
}

void LoadDetails(const std::wstring& fullPath, LibraryDetails& details)
{
    details = LibraryDetails();
    if (!FillEntryFromPath(fullPath, details.entry))
    {
        const size_t slash = fullPath.find_last_of(L'\\');
        details.entry.folder = slash == std::wstring::npos ? std::wstring() : fullPath.substr(0, slash);
        details.entry.name = slash == std::wstring::npos ? fullPath : fullPath.substr(slash + 1);
    }
    LibraryEntry& entry = details.entry;

    if (LibArchiveReader::IsArchive(fullPath))
    {
        details.isArchive = true;
        LibArchiveReader archive;
        if (!archive.Load(fullPath))
        {
            entry.type = L"Library";
            entry.error = archive.GetError();
            return;
        }
        details.isImportLibrary = archive.IsImportLibrary();
        entry.type = details.isImportLibrary ? L"Import Library" : L"Static Library";
        entry.architecture = MachineName(archive.GetMachine());

        const auto& members = archive.GetMembers();

        // One row per object file (or per target DLL of an import library).
        std::map<std::string, size_t> rowForName;
        std::vector<size_t> rowForMember(members.size());
        for (size_t i = 0; i < members.size(); ++i)
        {
            const auto it = rowForName.find(members[i].name);
            if (it != rowForName.end())
            {
                rowForMember[i] = it->second;
                continue;
            }
            rowForMember[i] = details.dependencies.size();
            rowForName[members[i].name] = details.dependencies.size();
            DllDependency dep;
            dep.dllName = members[i].name;
            details.dependencies.push_back(std::move(dep));
        }

        details.exports.reserve(archive.GetSymbols().size());
        for (const ArchiveSymbol& symbol : archive.GetSymbols())
        {
            ExportedFunction func;
            func.name = symbol.name;
            func.demangledName = symbol.demangledName;
            if (symbol.memberIndex < members.size())
            {
                func.forwardName = members[symbol.memberIndex].name;
                ImportedFunction f;
                f.name = symbol.name;
                details.dependencies[rowForMember[symbol.memberIndex]].functions.push_back(std::move(f));
            }
            details.exports.push_back(std::move(func));
        }
        entry.exports = (long long)details.exports.size();
        entry.dependencies = (long long)members.size();
        return;
    }

    PeImage image;
    if (!image.Open(fullPath))
    {
        entry.type = L"Unrecognised";
        entry.error = image.Error();
        return;
    }

    details.isPe = true;
    details.headers = image.GetHeaders();
    details.isManaged = IsManaged(image);
    entry.type = PeType(entry.name, image, details.isManaged);
    entry.architecture = Bitness(details.headers);

    DllExportReader exports;
    if (exports.Load(image))
    {
        details.exports = exports.GetExports();
        details.exportedModuleName = exports.GetModuleName();
        entry.exports = (long long)details.exports.size();
    }
    else
        entry.error = exports.GetError();

    DllImportReader imports;
    if (imports.Load(image))
    {
        details.dependencies = imports.GetDependencies();
        entry.dependencies = (long long)details.dependencies.size();
    }
    else if (entry.error.empty())
        entry.error = imports.GetError();

    image.Close();

    details.hasVersion = FileVersionReader::Read(fullPath, details.version);
    if (details.hasVersion)
    {
        entry.version = !details.version.fixedFileVersion.empty() ? details.version.fixedFileVersion : details.version.fileVersion;
        entry.company = details.version.companyName;
    }
}

std::wstring MachineName(WORD machine)
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:   return L"x86";
    case IMAGE_FILE_MACHINE_AMD64:  return L"x64";
    case IMAGE_FILE_MACHINE_ARM64:  return L"ARM64";
    case 0xA641:                    return L"ARM64EC";   // IMAGE_FILE_MACHINE_ARM64EC
    case 0xA64E:                    return L"ARM64X";    // IMAGE_FILE_MACHINE_ARM64X
    case IMAGE_FILE_MACHINE_ARMNT:  return L"ARM (Thumb-2)";
    case IMAGE_FILE_MACHINE_ARM:    return L"ARM";
    case IMAGE_FILE_MACHINE_IA64:   return L"Itanium";
    case IMAGE_FILE_MACHINE_UNKNOWN: return L"Any";
    default:
    {
        wchar_t text[32] = {};
        swprintf_s(text, L"0x%04X", machine);
        return text;
    }
    }
}

std::wstring SubsystemName(WORD subsystem)
{
    switch (subsystem)
    {
    case IMAGE_SUBSYSTEM_NATIVE:              return L"Native (kernel or boot)";
    case IMAGE_SUBSYSTEM_WINDOWS_GUI:         return L"Windows GUI";
    case IMAGE_SUBSYSTEM_WINDOWS_CUI:         return L"Windows Console";
    case IMAGE_SUBSYSTEM_EFI_APPLICATION:     return L"EFI Application";
    case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER: return L"EFI Boot Driver";
    case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:  return L"EFI Runtime Driver";
    case IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION: return L"Windows Boot Application";
    default:
    {
        wchar_t text[32] = {};
        swprintf_s(text, L"Other (%u)", subsystem);
        return text;
    }
    }
}

std::wstring DllCharacteristicsText(WORD c)
{
    std::wstring text;
    const auto add = [&](WORD flag, const wchar_t* name) {
        if (c & flag)
        {
            if (!text.empty())
                text += L", ";
            text += name;
        }
    };
    add(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, L"ASLR");
    add(IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA, L"High-entropy ASLR");
    add(IMAGE_DLLCHARACTERISTICS_NX_COMPAT, L"DEP");
    add(IMAGE_DLLCHARACTERISTICS_GUARD_CF, L"Control Flow Guard");
    add(IMAGE_DLLCHARACTERISTICS_NO_SEH, L"No SEH");
    add(IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY, L"Force integrity");
    add(IMAGE_DLLCHARACTERISTICS_APPCONTAINER, L"AppContainer");
    return text.empty() ? L"None" : text;
}

std::wstring FileTimeText(ULONGLONG fileTimeUtc)
{
    if (!fileTimeUtc)
        return std::wstring();
    const FILETIME utc = { DWORD(fileTimeUtc), DWORD(fileTimeUtc >> 32) };
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
        return std::wstring();
    wchar_t text[32] = {};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return text;
}

std::wstring LinkTimeText(DWORD timeDateStamp)
{
    if (!timeDateStamp)
        return L"Not set";
    // Seconds since 1970 converted to FILETIME units.
    const std::wstring text = FileTimeText((ULONGLONG(timeDateStamp) + 11644473600ULL) * 10000000ULL);
    wchar_t raw[16] = {};
    swprintf_s(raw, L"0x%08X", timeDateStamp);
    return text + L"  (" + raw + L")";
}
