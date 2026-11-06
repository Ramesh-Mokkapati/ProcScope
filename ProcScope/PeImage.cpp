// PeImage.cpp
#include "PeImage.h"
#include <imagehlp.h>
#include <mutex>

#pragma comment(lib, "imagehlp.lib")

namespace
{
    // Reading a cloud placeholder would silently download the whole file.
    constexpr DWORD kNotOnDisk = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN;

    std::wstring SystemMessage(DWORD code)
    {
        wchar_t* buffer = nullptr;
        const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::wstring text = len ? std::wstring(buffer, len) : L"Windows error " + std::to_wstring(code);
        if (buffer)
            LocalFree(buffer);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.'))
            text.pop_back();
        return text;
    }
}

std::wstring ToExtendedPath(const std::wstring& path)
{
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    if (path.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::string DemangleSymbol(const std::string& decorated)
{
    // Import libraries prefix the decorated name, e.g. "__imp_?foo@@YAHXZ".
    static const std::string kImp = "__imp_";
    const size_t start = decorated.rfind(kImp, 0) == 0 ? kImp.size() : 0;
    if (start >= decorated.size() || decorated[start] != '?')
        return decorated;

    // DbgHelp is documented as single-threaded.
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);

    char buffer[4096] = {};
    if (UnDecorateSymbolName(decorated.c_str() + start, buffer, sizeof(buffer), UNDNAME_COMPLETE) == 0 || !buffer[0])
        return decorated;
    return buffer;
}

std::wstring ToWide(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    UINT codePage = CP_UTF8;
    int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (count == 0)
    {
        codePage = CP_ACP;
        count = MultiByteToWideChar(codePage, 0, text.data(), int(text.size()), nullptr, 0);
    }
    std::wstring out(size_t(count), L'\0');
    if (count)
        MultiByteToWideChar(codePage, 0, text.data(), int(text.size()), &out[0], count);
    return out;
}

bool RunGuardedRaw(void (*fn)(void*), void* context)
{
    // Only paging errors are swallowed: an access violation here would be a parser bug,
    // which the fuzz test in Tests\ReaderTests has to be able to see.
    __try
    {
        fn(context);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------
// MappedFile

bool MappedFile::Open(const std::wstring& path)
{
    Close();
    const std::wstring extended = ToExtendedPath(path);

    const DWORD attributes = GetFileAttributesW(extended.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & kNotOnDisk))
    {
        m_error = L"Online-only file: not analysed, because reading it would download it";
        return false;
    }

    m_file = CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file == INVALID_HANDLE_VALUE)
    {
        m_error = SystemMessage(GetLastError());
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(m_file, &size))
    {
        m_error = SystemMessage(GetLastError());
        Close();
        m_error = m_error.empty() ? L"Could not read the file size" : m_error;
        return false;
    }
    if (size.QuadPart == 0)
    {
        Close();
        m_error = L"The file is empty";
        return false;
    }
    if (ULONGLONG(size.QuadPart) > SIZE_MAX)
    {
        Close();
        m_error = L"The file is too large for this build (use the x64 version)";
        return false;
    }

    m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m_mapping)
        m_data = static_cast<const BYTE*>(MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0));
    if (!m_data)
    {
        const std::wstring message = SystemMessage(GetLastError());
        Close();
        m_error = message;
        return false;
    }
    m_size = size_t(size.QuadPart);
    return true;
}

void MappedFile::Close()
{
    if (m_data)
        UnmapViewOfFile(m_data);
    if (m_mapping)
        CloseHandle(m_mapping);
    if (m_file != INVALID_HANDLE_VALUE)
        CloseHandle(m_file);
    m_data = nullptr;
    m_mapping = nullptr;
    m_file = INVALID_HANDLE_VALUE;
    m_size = 0;
    m_error.clear();
}

// ---------------------------------------------------------------------------
// PeImage

void PeImage::Close()
{
    m_file.Close();
    m_sections = nullptr;
    m_directories = nullptr;
    m_directoryCount = 0;
    m_sizeOfHeaders = 0;
    m_fileAlignment = 0;
    m_headers = Headers();
    m_error.clear();
}

bool PeImage::Open(const std::wstring& path)
{
    Close();
    if (!m_file.Open(path))
    {
        m_error = m_file.Error();
        return false;
    }

    std::wstring error;
    const bool readable = RunGuarded([&] {
        const BYTE* base = m_file.Data();
        const size_t size = m_file.Size();

        if (size < sizeof(IMAGE_DOS_HEADER))
        {
            error = L"Not a PE file (too small)";
            return;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            error = L"Not a PE file (no MZ header)";
            return;
        }

        const LONG lfanew = dos->e_lfanew;
        if (lfanew < 0 || size_t(lfanew) > size || size - size_t(lfanew) < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))
        {
            error = L"16-bit or DOS executable (no PE header)";
            return;
        }

        const BYTE* nt = base + lfanew;
        const DWORD signature = *reinterpret_cast<const DWORD*>(nt);
        if (signature != IMAGE_NT_SIGNATURE)
        {
            const WORD shortSig = LOWORD(signature);
            error = shortSig == IMAGE_OS2_SIGNATURE    ? L"16-bit Windows (NE) module: exports are not supported"
                  : shortSig == IMAGE_OS2_SIGNATURE_LE ? L"16-bit VxD (LE) module: exports are not supported"
                                                       : L"16-bit or DOS executable (no PE header)";
            return;
        }

        const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(nt + sizeof(DWORD));
        const size_t optionalOffset = size_t(lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (size - optionalOffset < fileHeader->SizeOfOptionalHeader || fileHeader->SizeOfOptionalHeader < sizeof(WORD))
        {
            error = L"Corrupt PE file (truncated optional header)";
            return;
        }

        const BYTE* optional = base + optionalOffset;
        const WORD magic = *reinterpret_cast<const WORD*>(optional);
        size_t directoriesOffset = 0;   // within the optional header

        m_headers.machine = fileHeader->Machine;
        m_headers.characteristics = fileHeader->Characteristics;
        m_headers.sectionCount = fileHeader->NumberOfSections;
        m_headers.timeDateStamp = fileHeader->TimeDateStamp;

        // The two layouts differ, so the choice follows the file's own format and not this build's.
        if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && fileHeader->SizeOfOptionalHeader >= FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32, DataDirectory))
        {
            const auto* o = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optional);
            m_headers.is64 = false;
            m_headers.subsystem = o->Subsystem;
            m_headers.dllCharacteristics = o->DllCharacteristics;
            m_headers.entryPointRva = o->AddressOfEntryPoint;
            m_headers.sizeOfImage = o->SizeOfImage;
            m_headers.checkSum = o->CheckSum;
            m_headers.imageBase = o->ImageBase;
            m_sizeOfHeaders = o->SizeOfHeaders;
            m_fileAlignment = o->FileAlignment;
            m_directoryCount = o->NumberOfRvaAndSizes;
            directoriesOffset = FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32, DataDirectory);
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fileHeader->SizeOfOptionalHeader >= FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, DataDirectory))
        {
            const auto* o = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optional);
            m_headers.is64 = true;
            m_headers.subsystem = o->Subsystem;
            m_headers.dllCharacteristics = o->DllCharacteristics;
            m_headers.entryPointRva = o->AddressOfEntryPoint;
            m_headers.sizeOfImage = o->SizeOfImage;
            m_headers.checkSum = o->CheckSum;
            m_headers.imageBase = o->ImageBase;
            m_sizeOfHeaders = o->SizeOfHeaders;
            m_fileAlignment = o->FileAlignment;
            m_directoryCount = o->NumberOfRvaAndSizes;
            directoriesOffset = FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, DataDirectory);
        }
        else
        {
            error = L"Corrupt PE file (unknown optional header)";
            return;
        }

        // Only the directories that really fit in the declared optional header count.
        const size_t room = (fileHeader->SizeOfOptionalHeader - directoriesOffset) / sizeof(IMAGE_DATA_DIRECTORY);
        if (m_directoryCount > room)
            m_directoryCount = DWORD(room);
        m_directories = reinterpret_cast<const IMAGE_DATA_DIRECTORY*>(optional + directoriesOffset);

        const size_t sectionsOffset = optionalOffset + fileHeader->SizeOfOptionalHeader;
        const size_t sectionBytes = size_t(fileHeader->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (sectionsOffset > size || size - sectionsOffset < sectionBytes)
        {
            error = L"Corrupt PE file (truncated section table)";
            return;
        }
        m_sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + sectionsOffset);
    });

    if (!readable)
        error = L"The file could not be read (I/O error)";
    if (!error.empty())
    {
        m_file.Close();
        m_sections = nullptr;
        m_directories = nullptr;
        m_directoryCount = 0;
        m_headers = Headers();
        m_error = error;
        return false;
    }
    return true;
}

IMAGE_DATA_DIRECTORY PeImage::Directory(UINT index) const
{
    if (index < m_directoryCount && m_directories)
        return m_directories[index];
    return IMAGE_DATA_DIRECTORY{};
}

bool PeImage::RvaToOffset(DWORD rva, size_t& offset) const
{
    if (rva < m_sizeOfHeaders)
    {
        offset = rva;
        return true;
    }
    for (WORD i = 0; i < m_headers.sectionCount; ++i)
    {
        const IMAGE_SECTION_HEADER& s = m_sections[i];
        if (rva < s.VirtualAddress)
            continue;
        const DWORD delta = rva - s.VirtualAddress;
        const DWORD span = s.Misc.VirtualSize ? max(s.Misc.VirtualSize, s.SizeOfRawData) : s.SizeOfRawData;
        if (delta >= span)
            continue;
        if (delta >= s.SizeOfRawData)
            return false;   // zero-filled at run time: not present in the file
        // The loader rounds the raw pointer down to a sector unless the file alignment is smaller.
        const DWORD rawStart = m_fileAlignment >= 0x200 ? (s.PointerToRawData & ~DWORD(0x1FF)) : s.PointerToRawData;
        offset = size_t(rawStart) + delta;
        return true;
    }
    return false;
}

const BYTE* PeImage::Ptr(DWORD rva, size_t bytes) const
{
    size_t offset = 0;
    const size_t size = m_file.Size();
    if (!m_file.Data() || !RvaToOffset(rva, offset) || offset > size || size - offset < bytes)
        return nullptr;
    return m_file.Data() + offset;
}

bool PeImage::FindSection(const char* name, const BYTE*& data, size_t& size) const
{
    for (WORD i = 0; i < m_headers.sectionCount; ++i)
    {
        const IMAGE_SECTION_HEADER& s = m_sections[i];
        if (strncmp(reinterpret_cast<const char*>(s.Name), name, IMAGE_SIZEOF_SHORT_NAME) != 0)
            continue;
        const size_t rawStart = m_fileAlignment >= 0x200 ? (s.PointerToRawData & ~DWORD(0x1FF)) : s.PointerToRawData;
        if (rawStart >= m_file.Size())
            return false;
        size_t length = s.SizeOfRawData;
        if (s.Misc.VirtualSize && s.Misc.VirtualSize < length)
            length = s.Misc.VirtualSize;
        if (length > m_file.Size() - rawStart)
            length = m_file.Size() - rawStart;
        data = m_file.Data() + rawStart;
        size = length;
        return true;
    }
    return false;
}

bool PeImage::ReadString(DWORD rva, std::string& out, size_t maxLen) const
{
    size_t offset = 0;
    const size_t size = m_file.Size();
    if (!m_file.Data() || !RvaToOffset(rva, offset) || offset >= size)
        return false;
    const char* text = reinterpret_cast<const char*>(m_file.Data() + offset);
    const size_t limit = min(maxLen, size - offset);
    const size_t length = strnlen(text, limit);
    if (length == limit)
        return false;
    out.assign(text, length);
    return true;
}
