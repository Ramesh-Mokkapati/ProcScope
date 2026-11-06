// FileSignatureVerifier.h : Authenticode verification, embedded and catalog.
// Public API from pe_analyzer_classes; see README for what changed underneath it.
#pragma once

#include <windows.h>
#include <string>

struct SignatureInfo {
    bool valid = false;
    bool catalogSigned = false;        // signed through a system catalog, not inside the file
    bool revocationChecked = false;    // false: valid, but revocation data wasn't available offline
    LONG status = 0;                   // final WinVerifyTrust result
    std::wstring message;
    std::wstring signer;
    std::wstring issuer;
    std::wstring catalogPath;
    FILETIME verifiedAsOf = {};        // signing timestamp when there is one
};

class FileSignatureVerifier {
public:
    static bool Verify(const std::wstring& filePath, std::wstring& outErrorMsg);
    static bool Verify(const std::wstring& filePath, SignatureInfo& info);
};
