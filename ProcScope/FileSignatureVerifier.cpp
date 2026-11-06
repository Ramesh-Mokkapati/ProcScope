// FileSignatureVerifier.cpp
#include "FileSignatureVerifier.h"
#include "PeImage.h"
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <cstdio>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
    std::wstring CertName(PCCERT_CONTEXT cert, DWORD flags)
    {
        wchar_t name[512] = {};
        if (CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, name, _countof(name)) <= 1)
            return std::wstring();
        return name;
    }

    // Runs one WinVerifyTrust pass. Revocation data comes from the local cache only, so
    // scanning a folder never goes out to the network.
    LONG RunTrust(WINTRUST_DATA& trustData, DWORD revocationChecks, SignatureInfo* info)
    {
        GUID actionId = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        trustData.cbStruct = sizeof(WINTRUST_DATA);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = revocationChecks;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.hWVTStateData = nullptr;
        trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

        const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &actionId, &trustData);

        if (info && trustData.hWVTStateData)
        {
            if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trustData.hWVTStateData))
                if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
                {
                    info->verifiedAsOf = signer->sftVerifyAsOf;
                    if (CRYPT_PROVIDER_CERT* cert = WTHelperGetProvCertFromChain(signer, 0))
                        if (cert->pCert)
                        {
                            info->signer = CertName(cert->pCert, 0);
                            info->issuer = CertName(cert->pCert, CERT_NAME_ISSUER_FLAG);
                        }
                }
        }

        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &actionId, &trustData);
        return result;
    }

    bool IsNoSignature(LONG result)
    {
        return result == TRUST_E_NOSIGNATURE || result == TRUST_E_SUBJECT_FORM_UNKNOWN || result == TRUST_E_PROVIDER_UNKNOWN;
    }

    bool IsRevocationUnknown(LONG result)
    {
        return result == CRYPT_E_REVOCATION_OFFLINE || result == CRYPT_E_NO_REVOCATION_CHECK || result == CERT_E_REVOCATION_FAILURE;
    }

    // A verdict counts as valid if only the revocation lookup was inconclusive.
    LONG VerifyWithRevocationFallback(WINTRUST_DATA& trustData, SignatureInfo& info)
    {
        LONG result = RunTrust(trustData, WTD_REVOKE_WHOLECHAIN, &info);
        info.revocationChecked = result == ERROR_SUCCESS;
        if (IsRevocationUnknown(result))
            result = RunTrust(trustData, WTD_REVOKE_NONE, &info);
        return result;
    }

    LONG VerifyEmbedded(const std::wstring& path, SignatureInfo& info)
    {
        WINTRUST_FILE_INFO fileInfo = {};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = path.c_str();

        WINTRUST_DATA trustData = {};
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        return VerifyWithRevocationFallback(trustData, info);
    }

    // Most Windows components carry no signature of their own: their hash is listed in a catalog.
    // Returns false if no catalog lists this file.
    bool VerifyCatalog(const std::wstring& path, LPCWSTR hashAlgorithm, SignatureInfo& info, LONG& result)
    {
        const std::wstring extended = ToExtendedPath(path);
        HANDLE file = CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        HCATADMIN admin = nullptr;
        bool found = false;
        if (CryptCATAdminAcquireContext2(&admin, nullptr, hashAlgorithm, nullptr, 0))
        {
            DWORD hashSize = 0;
            CryptCATAdminCalcHashFromFileHandle2(admin, file, &hashSize, nullptr, 0);
            std::vector<BYTE> hash(hashSize);
            if (hashSize && CryptCATAdminCalcHashFromFileHandle2(admin, file, &hashSize, hash.data(), 0))
            {
                HCATINFO catalog = CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hashSize, 0, nullptr);
                if (catalog)
                {
                    CATALOG_INFO catalogInfo = {};
                    catalogInfo.cbStruct = sizeof(catalogInfo);
                    if (CryptCATCatalogInfoFromContext(catalog, &catalogInfo, 0))
                    {
                        std::wstring tag;
                        for (BYTE b : hash)
                        {
                            wchar_t hex[3];
                            swprintf_s(hex, L"%02X", b);
                            tag += hex;
                        }

                        WINTRUST_CATALOG_INFO member = {};
                        member.cbStruct = sizeof(member);
                        member.pcwszCatalogFilePath = catalogInfo.wszCatalogFile;
                        member.pcwszMemberTag = tag.c_str();
                        member.pcwszMemberFilePath = path.c_str();
                        member.hMemberFile = file;
                        member.pbCalculatedFileHash = hash.data();
                        member.cbCalculatedFileHash = hashSize;
                        member.hCatAdmin = admin;

                        WINTRUST_DATA trustData = {};
                        trustData.dwUnionChoice = WTD_CHOICE_CATALOG;
                        trustData.pCatalog = &member;

                        result = VerifyWithRevocationFallback(trustData, info);
                        info.catalogSigned = true;
                        info.catalogPath = catalogInfo.wszCatalogFile;
                        found = true;
                    }
                    CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
                }
            }
            CryptCATAdminReleaseContext(admin, 0);
        }
        CloseHandle(file);
        return found;
    }

    std::wstring Describe(const SignatureInfo& info)
    {
        wchar_t code[96] = {};
        switch (info.status)
        {
        case ERROR_SUCCESS:
        {
            std::wstring text = info.catalogSigned ? L"The file is catalog-signed and the signature is valid."
                                                   : L"The file is signed and the signature is valid.";
            if (!info.revocationChecked)
                text += L" Revocation status wasn't available offline, so it was not checked.";
            return text;
        }
        case TRUST_E_NOSIGNATURE:
        case TRUST_E_SUBJECT_FORM_UNKNOWN:
        case TRUST_E_PROVIDER_UNKNOWN:
            return L"The file is not digitally signed.";
        case TRUST_E_BAD_DIGEST:
            return L"The signature does not match: the file was changed after it was signed.";
        case TRUST_E_EXPLICIT_DISTRUST:
            return L"The publisher's certificate is explicitly distrusted on this computer.";
        case TRUST_E_SUBJECT_NOT_TRUSTED:
            return L"The signature was present, but the chain was not trusted by the local machine.";
        case CRYPT_E_SECURITY_SETTINGS:
            return L"A security setting prevented the verification (e.g. untrusted root policy).";
        case CERT_E_REVOKED:
        case CRYPT_E_REVOKED:
            return L"The signing certificate has been revoked.";
        case CERT_E_EXPIRED:
            return L"The signing certificate has expired and the signature has no valid timestamp.";
        case CERT_E_UNTRUSTEDROOT:
            return L"The signature chains to a root certificate this computer does not trust.";
        case CERT_E_CHAINING:
            return L"The signing certificate's chain could not be built.";
        default:
            swprintf_s(code, L"Signature verification failed with error code: 0x%08X", static_cast<unsigned>(info.status));
            return code;
        }
    }
}

bool FileSignatureVerifier::Verify(const std::wstring& filePath, SignatureInfo& info)
{
    info = SignatureInfo();

    LONG result = VerifyEmbedded(filePath, info);
    if (IsNoSignature(result))
    {
        // Windows 10+ catalogs are SHA-256; older ones (and some drivers) are still SHA-1.
        if (!VerifyCatalog(filePath, BCRYPT_SHA256_ALGORITHM, info, result))
            VerifyCatalog(filePath, BCRYPT_SHA1_ALGORITHM, info, result);
    }

    info.status = result;
    info.valid = result == ERROR_SUCCESS;
    info.message = Describe(info);
    return info.valid;
}

bool FileSignatureVerifier::Verify(const std::wstring& filePath, std::wstring& outErrorMsg)
{
    SignatureInfo info;
    const bool valid = Verify(filePath, info);
    outErrorMsg = info.message;
    return valid;
}
