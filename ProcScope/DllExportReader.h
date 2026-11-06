// DllExportReader.h : exported functions of a PE file.
// Public API from pe_analyzer_classes; see README for what changed underneath it.
#pragma once

#include <windows.h>
#include <vector>
#include <string>

class PeImage;

struct ExportedFunction {
    std::string name;
    std::string demangledName;
    WORD ordinal = 0;
    DWORD rva = 0;
    bool isForwarded = false;
    std::string forwardName;
};

class DllExportReader {
public:
    DllExportReader();
    ~DllExportReader();

    // Maps the file read-only and parses its exports without loading it, so no DllMain runs and
    // any bitness can be read. Returns true (with no exports) for a valid PE that exports nothing.
    bool Load(const std::wstring& dllPath);
    void Unload();

    // Same parse on an image that is already open.
    bool Load(const PeImage& image);

    // Retrieves the list of parsed exports
    const std::vector<ExportedFunction>& GetExports() const { return m_exports; }

    // Module name recorded in the export directory (may differ from the file name).
    const std::string& GetModuleName() const { return m_moduleName; }

    // Why Load returned false.
    const std::wstring& GetError() const { return m_error; }

    // Number of exports, without building the strings; -1 if the directory is corrupt.
    static long long Count(const PeImage& image);

private:
    std::vector<ExportedFunction> m_exports;
    std::string m_moduleName;
    std::wstring m_error;
};
