// PeImage.h : read-only, bounds-checked access to PE files and other binaries on disk.
//
// Nothing here loads a module into the process: no DllMain, no loader-lock, no bitness limit.
// Every offset read from a file is validated, because a folder scan meets corrupt and hostile files.
#pragma once

#include <windows.h>
#include <string>
#include <type_traits>

// "\\?\C:\..." or "\\?\UNC\server\share\..." so paths beyond MAX_PATH can be opened.
std::wstring ToExtendedPath(const std::wstring& path);

// Undecorates MSVC C++ names ("?foo@@YAHXZ"); anything else comes back unchanged.
std::string DemangleSymbol(const std::string& decorated);

// Converts a name from a binary to UTF-16 (UTF-8 if valid, otherwise the ANSI code page).
std::wstring ToWide(const std::string& text);

// Runs fn, turning a paging failure on mapped data (file truncated, network lost) into `false`.
bool RunGuardedRaw(void (*fn)(void*), void* context);

template <class F>
bool RunGuarded(F&& f)
{
    using Fn = std::remove_reference_t<F>;
    return RunGuardedRaw([](void* c) { (*static_cast<Fn*>(c))(); }, &f);
}

class MappedFile
{
public:
    MappedFile() = default;
    ~MappedFile() { Close(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(const std::wstring& path);
    void Close();

    const BYTE*         Data() const { return m_data; }
    size_t              Size() const { return m_size; }
    const std::wstring& Error() const { return m_error; }

private:
    HANDLE       m_file = INVALID_HANDLE_VALUE;
    HANDLE       m_mapping = nullptr;
    const BYTE*  m_data = nullptr;
    size_t       m_size = 0;
    std::wstring m_error;
};

class PeImage
{
public:
    struct Headers
    {
        WORD      machine = 0;
        WORD      characteristics = 0;
        WORD      subsystem = 0;
        WORD      dllCharacteristics = 0;
        WORD      sectionCount = 0;
        DWORD     timeDateStamp = 0;
        DWORD     entryPointRva = 0;
        DWORD     sizeOfImage = 0;
        DWORD     checkSum = 0;
        ULONGLONG imageBase = 0;
        bool      is64 = false;
    };

    // Maps the file and validates its headers. Fails with Error() on anything that is not a PE file.
    bool Open(const std::wstring& path);
    void Close();

    const std::wstring& Error() const { return m_error; }
    const Headers&      GetHeaders() const { return m_headers; }
    bool                Is64() const { return m_headers.is64; }

    // Returns an empty directory when the index is beyond NumberOfRvaAndSizes.
    IMAGE_DATA_DIRECTORY Directory(UINT index) const;

    // Pointer to `bytes` bytes at an RVA, or nullptr when any part of it lies outside the file.
    const BYTE* Ptr(DWORD rva, size_t bytes) const;

    template <class T>
    const T* At(DWORD rva, size_t count = 1) const
    {
        if (count > SIZE_MAX / sizeof(T))
            return nullptr;
        return reinterpret_cast<const T*>(Ptr(rva, count * sizeof(T)));
    }

    // Reads a NUL-terminated string; false if it starts outside the file or never terminates.
    bool ReadString(DWORD rva, std::string& out, size_t maxLen = 8192) const;

    // The file bytes of a section by name (such as ".apiset"), clamped to the file.
    bool FindSection(const char* name, const BYTE*& data, size_t& size) const;

private:
    bool RvaToOffset(DWORD rva, size_t& offset) const;

    MappedFile                   m_file;
    const IMAGE_SECTION_HEADER*  m_sections = nullptr;
    const IMAGE_DATA_DIRECTORY*  m_directories = nullptr;
    DWORD                        m_directoryCount = 0;
    DWORD                        m_sizeOfHeaders = 0;
    DWORD                        m_fileAlignment = 0;
    Headers                      m_headers;
    std::wstring                 m_error;
};
