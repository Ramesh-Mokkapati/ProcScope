// LibraryAnalyzer.h : combines the readers into what the UI shows.
//   AnalyzeQuick - cheap facts for one row of a whole-drive scan (any thread).
//   LoadDetails  - everything except the signature, which is slow and loaded separately.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "DllExportReader.h"
#include "DllImportReader.h"
#include "FileVersionReader.h"
#include "PeImage.h"

struct LibraryEntry
{
    std::wstring name;
    std::wstring folder;
    ULONGLONG    size = 0;
    ULONGLONG    modified = 0;       // FILETIME (UTC)

    std::wstring type;
    std::wstring architecture;
    long long    exports = -1;       // -1: not available
    long long    dependencies = -1;
    std::wstring version;
    std::wstring company;
    std::wstring error;              // why the file could not be analysed

    std::wstring FullPath() const;
};

struct LibraryDetails
{
    LibraryEntry                  entry;
    bool                          isPe = false;
    bool                          isArchive = false;
    bool                          isImportLibrary = false;
    bool                          isManaged = false;
    PeImage::Headers              headers;
    std::string                   exportedModuleName;
    std::vector<ExportedFunction> exports;      // for an archive: its public symbols
    std::vector<DllDependency>    dependencies; // for an archive: members and their symbols
    bool                          hasVersion = false;
    VersionInfo                   version;
};

// True for a file name this application treats as a library.
bool IsLibraryFileName(const std::wstring& fileName, bool includeExecutables);

// Lowercase file extensions recognised as libraries, for display ("dll, ocx, ...").
std::wstring LibraryExtensionList();

void AnalyzeQuick(LibraryEntry& entry);
void LoadDetails(const std::wstring& fullPath, LibraryDetails& details);

// Fills name, folder, size and modified time from disk; false if the file can't be found.
bool FillEntryFromPath(const std::wstring& fullPath, LibraryEntry& entry);

std::wstring MachineName(WORD machine);
std::wstring SubsystemName(WORD subsystem);
std::wstring DllCharacteristicsText(WORD characteristics);
std::wstring FileTimeText(ULONGLONG fileTimeUtc);
std::wstring LinkTimeText(DWORD timeDateStamp);
