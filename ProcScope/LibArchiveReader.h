// LibArchiveReader.h : COFF/GNU "ar" archives (.lib, .a): static and import libraries.
// These don't export functions the way a DLL does; the equivalent is their public symbol index.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct ArchiveSymbol {
    std::string name;
    std::string demangledName;
    size_t      memberIndex = size_t(-1);   // index into GetMembers()
};

struct ArchiveMember {
    std::string name;            // object file, or the DLL for an import member
    bool        isImport = false;
    WORD        machine = 0;
    ULONGLONG   size = 0;
};

class LibArchiveReader {
public:
    // True if the file starts with an archive signature.
    static bool IsArchive(const std::wstring& path);

    bool Load(const std::wstring& path);
    void Unload();

    const std::vector<ArchiveSymbol>& GetSymbols() const { return m_symbols; }
    const std::vector<ArchiveMember>& GetMembers() const { return m_members; }
    const std::wstring& GetError() const { return m_error; }

    bool IsImportLibrary() const { return m_importMembers > 0 && m_importMembers * 2 >= m_members.size(); }
    WORD GetMachine() const { return m_machine; }

private:
    std::vector<ArchiveSymbol> m_symbols;
    std::vector<ArchiveMember> m_members;
    std::wstring m_error;
    size_t m_importMembers = 0;
    WORD m_machine = 0;
};
