// LibArchiveReader.cpp
#include "LibArchiveReader.h"
#include "PeImage.h"
#include <cstring>
#include <unordered_map>

namespace
{
    const char kArchiveMagic[] = "!<arch>\n";
    const size_t kMagicSize = 8;

#pragma pack(push, 1)
    struct MemberHeader
    {
        char name[16];
        char date[12];
        char user[6];
        char group[6];
        char mode[8];
        char size[10];
        char end[2];
    };
#pragma pack(pop)

    DWORD ReadBigEndian32(const BYTE* p)
    {
        return (DWORD(p[0]) << 24) | (DWORD(p[1]) << 16) | (DWORD(p[2]) << 8) | DWORD(p[3]);
    }

    bool ParseSize(const MemberHeader& h, ULONGLONG& out)
    {
        out = 0;
        bool any = false;
        for (char c : h.size)
        {
            if (c < '0' || c > '9')
                break;
            out = out * 10 + ULONGLONG(c - '0');
            any = true;
        }
        return any;
    }

    std::string TrimmedName(const MemberHeader& h)
    {
        size_t n = sizeof(h.name);
        while (n > 0 && h.name[n - 1] == ' ')
            --n;
        return std::string(h.name, n);
    }

    std::string BaseName(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Symbols in the index, each with the file offset of the member that defines it.
    struct IndexEntry
    {
        std::string name;
        DWORD       memberOffset;
    };

    // Reads `count` NUL-terminated strings from [p, end). Stops early on truncation.
    template <class Store>
    void ReadStrings(const BYTE* p, const BYTE* end, size_t count, Store store)
    {
        for (size_t i = 0; i < count && p < end; ++i)
        {
            const char* text = reinterpret_cast<const char*>(p);
            const size_t length = strnlen(text, size_t(end - p));
            if (p + length >= end)
                break;
            store(i, std::string(text, length));
            p += length + 1;
        }
    }

    // First linker member: big-endian count, big-endian offsets, strings. This is also GNU's format.
    void ParseFirstLinker(const BYTE* data, size_t size, std::vector<IndexEntry>& out)
    {
        if (size < 4)
            return;
        const size_t count = ReadBigEndian32(data);
        if (count > (size - 4) / 4)
            return;
        const BYTE* offsets = data + 4;
        out.resize(count);
        size_t filled = 0;
        ReadStrings(offsets + count * 4, data + size, count, [&](size_t i, std::string text) {
            out[i].name = std::move(text);
            out[i].memberOffset = ReadBigEndian32(offsets + i * 4);
            filled = i + 1;
        });
        out.resize(filled);
    }

    // Second linker member (Microsoft only): little-endian, with indices into the member offset table.
    bool ParseSecondLinker(const BYTE* data, size_t size, std::vector<IndexEntry>& out)
    {
        if (size < 8)
            return false;
        const DWORD memberCount = *reinterpret_cast<const DWORD*>(data);
        if (memberCount > (size - 8) / 4)
            return false;
        const BYTE* afterOffsets = data + 4 + size_t(memberCount) * 4;
        const size_t remaining = size - 4 - size_t(memberCount) * 4;
        if (remaining < 4)
            return false;
        const DWORD symbolCount = *reinterpret_cast<const DWORD*>(afterOffsets);
        if (symbolCount > (remaining - 4) / 2)
            return false;

        const DWORD* offsets = reinterpret_cast<const DWORD*>(data + 4);
        const WORD* indices = reinterpret_cast<const WORD*>(afterOffsets + 4);
        out.resize(symbolCount);
        size_t filled = 0;
        ReadStrings(reinterpret_cast<const BYTE*>(indices + symbolCount), data + size, symbolCount, [&](size_t i, std::string text) {
            const WORD index = indices[i];
            out[i].name = std::move(text);
            out[i].memberOffset = index >= 1 && index <= memberCount ? offsets[index - 1] : 0;
            filled = i + 1;
        });
        out.resize(filled);
        return true;
    }
}

bool LibArchiveReader::IsArchive(const std::wstring& path)
{
    HANDLE file = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char magic[kMagicSize] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, magic, kMagicSize, &read, nullptr);
    CloseHandle(file);
    return ok && read == kMagicSize && memcmp(magic, kArchiveMagic, kMagicSize) == 0;
}

void LibArchiveReader::Unload()
{
    m_symbols.clear();
    m_members.clear();
    m_error.clear();
    m_importMembers = 0;
    m_machine = 0;
}

bool LibArchiveReader::Load(const std::wstring& path)
{
    Unload();

    MappedFile file;
    if (!file.Open(path))
    {
        m_error = file.Error();
        return false;
    }

    const bool readable = RunGuarded([&] {
        const BYTE* base = file.Data();
        const size_t size = file.Size();
        if (size < kMagicSize || memcmp(base, kArchiveMagic, kMagicSize) != 0)
        {
            m_error = L"Not a library archive";
            return;
        }

        const char* longNames = nullptr;
        size_t longNamesSize = 0;
        std::vector<IndexEntry> index;
        int linkerMembers = 0;
        std::unordered_map<ULONGLONG, size_t> memberAtOffset;

        size_t offset = kMagicSize;
        while (offset <= size && size - offset >= sizeof(MemberHeader))
        {
            const auto* header = reinterpret_cast<const MemberHeader*>(base + offset);
            ULONGLONG dataSize = 0;
            if (header->end[0] != '`' || header->end[1] != '\n' || !ParseSize(*header, dataSize))
            {
                // Keep what was read so far; a damaged tail shouldn't hide the valid part.
                if (m_members.empty() && index.empty())
                    m_error = L"Corrupt library archive";
                break;
            }

            const size_t dataOffset = offset + sizeof(MemberHeader);
            const size_t available = size - dataOffset;
            const size_t usable = dataSize > available ? available : size_t(dataSize);
            const BYTE* data = base + dataOffset;
            const std::string rawName = TrimmedName(*header);

            if (rawName == "/" || rawName == "/SYM64/")
            {
                // The Microsoft second linker member is preferred: its strings are the sorted set.
                std::vector<IndexEntry> entries;
                if (rawName == "/" && ++linkerMembers == 2)
                {
                    if (ParseSecondLinker(data, usable, entries))
                        index = std::move(entries);
                }
                else if (rawName == "/")
                {
                    ParseFirstLinker(data, usable, entries);
                    index = std::move(entries);
                }
            }
            else if (rawName == "//")
            {
                longNames = reinterpret_cast<const char*>(data);
                longNamesSize = usable;
            }
            else
            {
                ArchiveMember member;
                member.size = dataSize;

                if (rawName.size() > 1 && rawName[0] == '/' && longNames)
                {
                    const size_t nameOffset = strtoul(rawName.c_str() + 1, nullptr, 10);
                    if (nameOffset < longNamesSize)
                    {
                        const char* text = longNames + nameOffset;
                        size_t length = strnlen(text, longNamesSize - nameOffset);
                        // GNU ends long names with "/\n" rather than NUL.
                        const void* newline = memchr(text, '\n', length);
                        if (newline)
                            length = size_t(static_cast<const char*>(newline) - text);
                        if (length && text[length - 1] == '/')
                            --length;
                        member.name.assign(text, length);
                    }
                }
                else
                {
                    member.name = rawName;
                    if (!member.name.empty() && member.name.back() == '/')
                        member.name.pop_back();
                }

                // IMPORT_OBJECT_HEADER: Sig1 = 0, Sig2 = 0xFFFF, Version = 0 (other versions are /GL objects).
                if (usable >= sizeof(IMPORT_OBJECT_HEADER))
                {
                    const auto* import = reinterpret_cast<const IMPORT_OBJECT_HEADER*>(data);
                    if (import->Sig1 == IMAGE_FILE_MACHINE_UNKNOWN && import->Sig2 == IMPORT_OBJECT_HDR_SIG2)
                    {
                        if (import->Version == 0)
                        {
                            member.isImport = true;
                            member.machine = import->Machine;
                            ++m_importMembers;

                            // symbol\0dll\0 follows the header.
                            const char* strings = reinterpret_cast<const char*>(import + 1);
                            const size_t room = usable - sizeof(IMPORT_OBJECT_HEADER);
                            const size_t symbolLength = strnlen(strings, room);
                            if (symbolLength < room)
                            {
                                const char* dll = strings + symbolLength + 1;
                                member.name.assign(dll, strnlen(dll, room - symbolLength - 1));
                            }
                        }
                    }
                    else
                    {
                        member.machine = *reinterpret_cast<const WORD*>(data);   // IMAGE_FILE_HEADER.Machine
                    }
                }

                member.name = BaseName(member.name);
                if (!m_machine && member.machine)
                    m_machine = member.machine;
                memberAtOffset[offset] = m_members.size();
                m_members.push_back(std::move(member));
            }

            // Members are padded to an even offset.
            if (dataSize > available)
                break;
            offset = dataOffset + usable + (usable & 1);
        }

        if (!m_error.empty())
            return;

        m_symbols.reserve(index.size());
        for (IndexEntry& entry : index)
        {
            ArchiveSymbol symbol;
            symbol.demangledName = DemangleSymbol(entry.name);
            symbol.name = std::move(entry.name);
            const auto it = memberAtOffset.find(entry.memberOffset);
            if (it != memberAtOffset.end())
                symbol.memberIndex = it->second;
            m_symbols.push_back(std::move(symbol));
        }
    });

    if (!readable)
    {
        Unload();
        m_error = L"The file could not be read (I/O error)";
        return false;
    }
    return m_error.empty();
}
