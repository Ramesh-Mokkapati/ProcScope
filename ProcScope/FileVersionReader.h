// FileVersionReader.h : version resource of a file.
// Public API from pe_analyzer_classes, plus the numeric versions and a fallback for files
// whose string table isn't listed under \VarFileInfo\Translation.
#pragma once

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

struct VersionInfo {
    std::wstring companyName;
    std::wstring fileDescription;
    std::wstring fileVersion;
    std::wstring internalName;
    std::wstring legalCopyright;
    std::wstring originalFilename;
    std::wstring productName;
    std::wstring productVersion;
    std::wstring fixedFileVersion;      // from VS_FIXEDFILEINFO, e.g. "10.0.26100.1"
    std::wstring fixedProductVersion;
    std::wstring language;
};

class FileVersionReader {
public:
    static bool Read(const std::wstring& filePath, VersionInfo& info) {
        DWORD dummy = 0;
        DWORD infoSize = GetFileVersionInfoSizeW(filePath.c_str(), &dummy);
        if (infoSize == 0) {
            return false;
        }

        std::vector<BYTE> buffer(infoSize);
        if (!GetFileVersionInfoW(filePath.c_str(), 0, infoSize, buffer.data())) {
            return false;
        }

        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT cbFixed = 0;
        if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &cbFixed) && fixed && cbFixed >= sizeof(VS_FIXEDFILEINFO)) {
            info.fixedFileVersion = FormatVersion(fixed->dwFileVersionMS, fixed->dwFileVersionLS);
            info.fixedProductVersion = FormatVersion(fixed->dwProductVersionMS, fixed->dwProductVersionLS);
        }

        struct LANGANDCODEPAGE {
            WORD wLanguage;
            WORD wCodePage;
        } *lpTranslate = nullptr;
        UINT cbTranslate = 0;

        // The usual English tables are tried as well: many files omit or misdeclare the translation.
        std::vector<LANGANDCODEPAGE> candidates;
        if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) && lpTranslate) {
            for (UINT i = 0; i < cbTranslate / sizeof(LANGANDCODEPAGE); ++i)
                candidates.push_back(lpTranslate[i]);
        }
        candidates.push_back({ 0x0409, 0x04B0 });
        candidates.push_back({ 0x0409, 0x04E4 });
        candidates.push_back({ 0x0000, 0x04B0 });

        wchar_t subBlock[64] = {};
        auto getString = [&](const wchar_t* key, std::wstring& outStr) {
            wchar_t* lpBuffer = nullptr;
            UINT cbLen = 0;
            std::wstring query = std::wstring(subBlock) + key;
            if (VerQueryValueW(buffer.data(), query.c_str(), reinterpret_cast<LPVOID*>(&lpBuffer), &cbLen) && lpBuffer && cbLen > 0) {
                outStr = lpBuffer;
                return true;
            }
            return false;
        };

        for (const LANGANDCODEPAGE& c : candidates) {
            swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\", c.wLanguage, c.wCodePage);
            std::wstring probe;
            if (!getString(L"FileVersion", probe) && !getString(L"ProductName", probe) && !getString(L"CompanyName", probe))
                continue;

            getString(L"CompanyName", info.companyName);
            getString(L"FileDescription", info.fileDescription);
            getString(L"FileVersion", info.fileVersion);
            getString(L"InternalName", info.internalName);
            getString(L"LegalCopyright", info.legalCopyright);
            getString(L"OriginalFilename", info.originalFilename);
            getString(L"ProductName", info.productName);
            getString(L"ProductVersion", info.productVersion);

            wchar_t language[128] = {};
            if (VerLanguageNameW(c.wLanguage, language, _countof(language)))
                info.language = language;
            break;
        }

        return true;
    }

private:
    static std::wstring FormatVersion(DWORD ms, DWORD ls) {
        wchar_t text[64] = {};
        swprintf_s(text, L"%u.%u.%u.%u", HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
        return text;
    }
};
