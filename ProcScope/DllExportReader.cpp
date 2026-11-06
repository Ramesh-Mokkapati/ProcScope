// DllExportReader.cpp
#include "DllExportReader.h"
#include "PeImage.h"

namespace
{
    bool IsForwarder(DWORD rva, const IMAGE_DATA_DIRECTORY& directory)
    {
        return rva >= directory.VirtualAddress && rva - directory.VirtualAddress < directory.Size;
    }

    void FillTarget(const PeImage& image, const IMAGE_DATA_DIRECTORY& directory, ExportedFunction& func)
    {
        if (IsForwarder(func.rva, directory))
        {
            func.isForwarded = true;
            image.ReadString(func.rva, func.forwardName);
        }
    }

    struct ExportTables
    {
        const IMAGE_EXPORT_DIRECTORY* directory = nullptr;
        const DWORD* functions = nullptr;
        const DWORD* names = nullptr;
        const WORD*  nameOrdinals = nullptr;
    };

    // False if a table lies outside the file.
    bool LocateTables(const PeImage& image, const IMAGE_DATA_DIRECTORY& data, ExportTables& t)
    {
        t.directory = image.At<IMAGE_EXPORT_DIRECTORY>(data.VirtualAddress);
        if (!t.directory)
            return false;
        t.functions = t.directory->NumberOfFunctions ? image.At<DWORD>(t.directory->AddressOfFunctions, t.directory->NumberOfFunctions) : nullptr;
        if (t.directory->NumberOfFunctions && !t.functions)
            return false;
        if (t.directory->NumberOfNames)
        {
            t.names = image.At<DWORD>(t.directory->AddressOfNames, t.directory->NumberOfNames);
            t.nameOrdinals = image.At<WORD>(t.directory->AddressOfNameOrdinals, t.directory->NumberOfNames);
            if (!t.names || !t.nameOrdinals)
                return false;
        }
        return true;
    }
}

DllExportReader::DllExportReader() = default;

DllExportReader::~DllExportReader() {
    Unload();
}

bool DllExportReader::Load(const std::wstring& dllPath) {
    Unload();
    PeImage image;
    if (!image.Open(dllPath)) {
        m_error = image.Error();
        return false;
    }
    return Load(image);
}

bool DllExportReader::Load(const PeImage& image) {
    Unload();

    bool ok = true;
    const bool readable = RunGuarded([&] {
        const IMAGE_DATA_DIRECTORY data = image.Directory(IMAGE_DIRECTORY_ENTRY_EXPORT);
        if (data.VirtualAddress == 0 || data.Size == 0)
            return;

        ExportTables t;
        if (!LocateTables(image, data, t)) {
            m_error = L"Corrupt export directory";
            ok = false;
            return;
        }

        image.ReadString(t.directory->Name, m_moduleName);

        const DWORD functionCount = t.directory->NumberOfFunctions;
        std::vector<bool> processedOrdinals(functionCount, false);
        m_exports.reserve(functionCount);

        for (DWORD i = 0; i < t.directory->NumberOfNames; ++i) {
            const DWORD functionIndex = t.nameOrdinals[i];
            if (functionIndex >= functionCount)
                continue;

            ExportedFunction func;
            image.ReadString(t.names[i], func.name);
            func.ordinal = WORD(functionIndex + t.directory->Base);
            func.rva = t.functions[functionIndex];
            FillTarget(image, data, func);
            func.demangledName = DemangleSymbol(func.name);

            m_exports.push_back(std::move(func));
            processedOrdinals[functionIndex] = true;
        }

        for (DWORD i = 0; i < functionCount; ++i) {
            if (processedOrdinals[i] || t.functions[i] == 0)
                continue;

            ExportedFunction func;
            func.ordinal = WORD(i + t.directory->Base);
            func.rva = t.functions[i];
            FillTarget(image, data, func);
            m_exports.push_back(std::move(func));
        }
    });

    if (!readable) {
        m_exports.clear();
        m_error = L"The file could not be read (I/O error)";
        return false;
    }
    return ok;
}

long long DllExportReader::Count(const PeImage& image) {
    long long count = 0;
    const bool readable = RunGuarded([&] {
        const IMAGE_DATA_DIRECTORY data = image.Directory(IMAGE_DIRECTORY_ENTRY_EXPORT);
        if (data.VirtualAddress == 0 || data.Size == 0)
            return;
        ExportTables t;
        if (!LocateTables(image, data, t)) {
            count = -1;
            return;
        }
        // Same rule as Load: gaps in the ordinal range are not exports.
        const DWORD functionCount = t.directory->NumberOfFunctions;
        std::vector<bool> named(functionCount, false);
        for (DWORD i = 0; i < t.directory->NumberOfNames; ++i) {
            const DWORD functionIndex = t.nameOrdinals[i];
            if (functionIndex < functionCount) {
                named[functionIndex] = true;
                ++count;
            }
        }
        for (DWORD i = 0; i < functionCount; ++i)
            if (!named[i] && t.functions[i] != 0)
                ++count;
    });
    return readable ? count : -1;
}

void DllExportReader::Unload() {
    m_exports.clear();
    m_moduleName.clear();
    m_error.clear();
}
