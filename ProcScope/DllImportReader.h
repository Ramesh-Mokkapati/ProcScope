// DllImportReader.h : DLLs a PE file depends on and the functions it imports from them.
// Public API from pe_analyzer_classes; see README for what changed underneath it.
#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "PeImage.h"

struct ImportedFunction {
    std::string name;
    WORD ordinal = 0;
    bool isOrdinal = false;
};

struct DllDependency {
    std::string dllName;
    std::vector<ImportedFunction> functions;
    bool isDelayLoaded = false;
};

class DllImportReader {
public:
    DllImportReader() = default;
    ~DllImportReader() { Unload(); }

    // Maps the file read-only; any bitness can be read and no code in it runs.
    bool Load(const std::wstring& dllPath) {
        Unload();
        PeImage image;
        if (!image.Open(dllPath)) {
            m_error = image.Error();
            return false;
        }
        return Load(image);
    }

    // Same parse on an image that is already open.
    bool Load(const PeImage& image) {
        Unload();
        const bool readable = RunGuarded([&] {
            ReadStaticImports(image);
            ReadDelayImports(image);
        });
        if (!readable) {
            m_dependencies.clear();
            m_error = L"The file could not be read (I/O error)";
            return false;
        }
        return true;
    }

    void Unload() {
        m_dependencies.clear();
        m_error.clear();
    }

    const std::vector<DllDependency>& GetDependencies() const { return m_dependencies; }
    const std::wstring& GetError() const { return m_error; }

    // Number of dependencies (static and delay-loaded) without reading the function lists.
    static long long CountDependencies(const PeImage& image) {
        long long count = 0;
        const bool readable = RunGuarded([&] {
            ForEachDescriptor<IMAGE_IMPORT_DESCRIPTOR>(image, IMAGE_DIRECTORY_ENTRY_IMPORT,
                [](const IMAGE_IMPORT_DESCRIPTOR& d) { return d.Name != 0; },
                [&](const IMAGE_IMPORT_DESCRIPTOR&) { ++count; });
            ForEachDescriptor<IMAGE_DELAYLOAD_DESCRIPTOR>(image, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
                [](const IMAGE_DELAYLOAD_DESCRIPTOR& d) { return d.DllNameRVA != 0; },
                [&](const IMAGE_DELAYLOAD_DESCRIPTOR&) { ++count; });
        });
        return readable ? count : -1;
    }

private:
    // Limits on tables terminated by a zero entry, so a corrupt file can't run the scan out of memory.
    static constexpr size_t kMaxDescriptors = 16384;
    static constexpr size_t kMaxThunks = 1u << 20;

    template <class Descriptor, class IsValid, class Visit>
    static void ForEachDescriptor(const PeImage& image, UINT directory, IsValid isValid, Visit visit) {
        const IMAGE_DATA_DIRECTORY data = image.Directory(directory);
        if (data.VirtualAddress == 0 || data.Size == 0)
            return;
        for (size_t i = 0; i < kMaxDescriptors; ++i) {
            const auto* d = image.At<Descriptor>(data.VirtualAddress + DWORD(i * sizeof(Descriptor)));
            if (!d || !isValid(*d))
                break;
            visit(*d);
        }
    }

    template <class Thunk>
    static void ReadThunks(const PeImage& image, DWORD thunkRva, ULONGLONG ordinalFlag, DllDependency& dep) {
        for (size_t i = 0; i < kMaxThunks; ++i) {
            const auto* thunk = image.At<Thunk>(thunkRva + DWORD(i * sizeof(Thunk)));
            if (!thunk || *thunk == 0)
                break;

            ImportedFunction func;
            if (ULONGLONG(*thunk) & ordinalFlag) {
                func.isOrdinal = true;
                func.ordinal = WORD(*thunk & 0xFFFF);
            } else {
                // IMAGE_IMPORT_BY_NAME: WORD hint, then the name.
                image.ReadString(DWORD(*thunk) + sizeof(WORD), func.name);
            }
            dep.functions.push_back(std::move(func));
        }
    }

    void ReadFunctions(const PeImage& image, DWORD thunkRva, DllDependency& dep) {
        if (thunkRva == 0)
            return;
        // Thunks follow the file's bitness, not this build's.
        if (image.Is64())
            ReadThunks<ULONGLONG>(image, thunkRva, IMAGE_ORDINAL_FLAG64, dep);
        else
            ReadThunks<DWORD>(image, thunkRva, IMAGE_ORDINAL_FLAG32, dep);
    }

    void ReadStaticImports(const PeImage& image) {
        ForEachDescriptor<IMAGE_IMPORT_DESCRIPTOR>(image, IMAGE_DIRECTORY_ENTRY_IMPORT,
            [](const IMAGE_IMPORT_DESCRIPTOR& d) { return d.Name != 0; },
            [&](const IMAGE_IMPORT_DESCRIPTOR& d) {
                DllDependency dep;
                image.ReadString(d.Name, dep.dllName);
                ReadFunctions(image, d.OriginalFirstThunk ? d.OriginalFirstThunk : d.FirstThunk, dep);
                m_dependencies.push_back(std::move(dep));
            });
    }

    void ReadDelayImports(const PeImage& image) {
        ForEachDescriptor<IMAGE_DELAYLOAD_DESCRIPTOR>(image, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
            [](const IMAGE_DELAYLOAD_DESCRIPTOR& d) { return d.DllNameRVA != 0; },
            [&](const IMAGE_DELAYLOAD_DESCRIPTOR& d) {
                // Old linkers stored virtual addresses here instead of RVAs.
                const DWORD adjust = d.Attributes.RvaBased ? 0 : DWORD(image.GetHeaders().imageBase);
                DllDependency dep;
                dep.isDelayLoaded = true;
                image.ReadString(d.DllNameRVA - adjust, dep.dllName);
                ReadFunctions(image, d.ImportNameTableRVA ? d.ImportNameTableRVA - adjust : 0, dep);
                m_dependencies.push_back(std::move(dep));
            });
    }

    std::vector<DllDependency> m_dependencies;
    std::wstring m_error;
};
