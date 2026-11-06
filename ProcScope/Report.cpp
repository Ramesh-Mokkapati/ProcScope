// Report.cpp
#include "pch.h"
#include "Report.h"
#include "Format.h"

namespace report
{
    namespace
    {
        void Add(std::vector<Property>& rows, LPCTSTR name, const CString& value)
        {
            if (!value.IsEmpty())
                rows.emplace_back(CString(name), value);
        }

        void Add(std::vector<Property>& rows, LPCTSTR name, const std::wstring& value)
        {
            Add(rows, name, fmt::Wide(value));
        }

        CString Hex(ULONGLONG value, int digits)
        {
            CString s;
            s.Format(_T("0x%0*I64X"), digits, value);
            return s;
        }

        CString Yes(bool value)
        {
            return value ? CString(_T("Yes")) : CString(_T("No"));
        }
    }

    bool WriteUtf8File(const CString& path, const CString& text)
    {
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), nullptr, 0, nullptr, nullptr);
        std::vector<char> utf8(size_t(bytes) + 3);
        utf8[0] = '\xEF'; utf8[1] = '\xBB'; utf8[2] = '\xBF';
        if (bytes)
            ::WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), utf8.data() + 3, bytes, nullptr, nullptr);

        HANDLE file = ::CreateFile(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const BOOL ok = ::WriteFile(file, utf8.data(), DWORD(utf8.size()), &written, nullptr);
        ::CloseHandle(file);
        return ok && written == utf8.size();
    }

    CString CsvField(const CString& value)
    {
        // A leading = + - @ would run as a formula when the CSV is opened in Excel, and C++ names can start with these.
        CString text(value);
        if (!text.IsEmpty() && _tcschr(_T("=+-@"), text[0]))
            text.Insert(0, _T('\''));
        if (text.FindOneOf(_T(",\"\r\n")) >= 0)
        {
            text.Replace(_T("\""), _T("\"\""));
            return _T("\"") + text + _T("\"");
        }
        return text;
    }

    CString ExportNameText(const ExportedFunction& e)
    {
        return e.name.empty() ? CString(_T("(ordinal only)")) : fmt::Wide(ToWide(e.name));
    }

    CString ImportNameText(const ImportedFunction& f)
    {
        if (f.isOrdinal)
        {
            CString s;
            s.Format(_T("(by ordinal %u)"), f.ordinal);
            return s;
        }
        return fmt::Wide(ToWide(f.name));
    }

    std::vector<Property> SummaryProperties(const LibraryDetails& d)
    {
        std::vector<Property> rows;
        const LibraryEntry& e = d.entry;

        Add(rows, _T("File name"), e.name);
        Add(rows, _T("Folder"), e.folder);
        Add(rows, _T("Problem"), e.error);
        Add(rows, _T("Type"), e.type);
        Add(rows, _T("Architecture"), e.architecture);
        if (e.size || !e.error.empty())
        {
            CString size;
            size.Format(_T("%s  (%s bytes)"), fmt::Bytes(double(e.size)).GetString(), fmt::Count(e.size).GetString());
            Add(rows, _T("Size"), size);
        }
        Add(rows, _T("Modified"), FileTimeText(e.modified));

        if (d.isArchive)
        {
            if (e.exports >= 0)
                Add(rows, _T("Public symbols"), fmt::Count(ULONGLONG(e.exports)));
            if (e.dependencies >= 0)
                Add(rows, d.isImportLibrary ? _T("Import members") : _T("Object members"), fmt::Count(ULONGLONG(e.dependencies)));
            if (d.isImportLibrary)
                Add(rows, _T("Note"), CString(_T("Import library: it holds no code. Its symbols link to the DLLs listed under Dependencies.")));
            return rows;
        }
        if (!d.isPe)
            return rows;

        size_t forwarded = 0, named = 0;
        for (const ExportedFunction& x : d.exports)
        {
            forwarded += x.isForwarded ? 1 : 0;
            named += x.name.empty() ? 0 : 1;
        }
        CString exportsText;
        exportsText.Format(_T("%s  (%s named, %s by ordinal only, %s forwarded)"), fmt::Count(d.exports.size()).GetString(),
                           fmt::Count(named).GetString(), fmt::Count(d.exports.size() - named).GetString(), fmt::Count(forwarded).GetString());
        Add(rows, _T("Exported functions"), exportsText);
        if (d.exports.empty())
            Add(rows, _T("Note"), d.isManaged
                ? CString(_T("Managed .NET assembly: its public types live in metadata, not in the export table."))
                : CString(_T("This file has no export table.")));
        Add(rows, _T("Export module name"), ToWide(d.exportedModuleName));

        size_t functions = 0, delayed = 0;
        for (const DllDependency& dep : d.dependencies)
        {
            functions += dep.functions.size();
            delayed += dep.isDelayLoaded ? 1 : 0;
        }
        CString depsText;
        depsText.Format(_T("%s DLLs  (%s delay-loaded), %s imported functions"), fmt::Count(d.dependencies.size()).GetString(),
                        fmt::Count(delayed).GetString(), fmt::Count(functions).GetString());
        Add(rows, _T("Dependencies"), depsText);

        const PeImage::Headers& h = d.headers;
        Add(rows, _T("Image format"), CString(h.is64 ? _T("PE32+ (64-bit)") : _T("PE32 (32-bit)")));
        Add(rows, _T("Managed (.NET)"), Yes(d.isManaged));
        Add(rows, _T("Is DLL"), Yes((h.characteristics & IMAGE_FILE_DLL) != 0));
        Add(rows, _T("Subsystem"), SubsystemName(h.subsystem));
        Add(rows, _T("Image base"), Hex(h.imageBase, h.is64 ? 16 : 8));
        Add(rows, _T("Size of image"), Hex(h.sizeOfImage, 8));
        Add(rows, _T("Entry point RVA"), Hex(h.entryPointRva, 8));
        Add(rows, _T("Sections"), fmt::Count(h.sectionCount));
        Add(rows, _T("Link timestamp"), LinkTimeText(h.timeDateStamp));
        Add(rows, _T("Header checksum"), Hex(h.checkSum, 8));
        Add(rows, _T("Security features"), DllCharacteristicsText(h.dllCharacteristics));
        return rows;
    }

    std::vector<Property> VersionProperties(const LibraryDetails& d)
    {
        std::vector<Property> rows;
        if (!d.hasVersion)
        {
            rows.emplace_back(CString(_T("Version information")), CString(_T("This file has no version resource.")));
            return rows;
        }
        const VersionInfo& v = d.version;
        Add(rows, _T("Company"), v.companyName);
        Add(rows, _T("Description"), v.fileDescription);
        Add(rows, _T("File version"), v.fileVersion);
        Add(rows, _T("File version (binary)"), v.fixedFileVersion);
        Add(rows, _T("Product name"), v.productName);
        Add(rows, _T("Product version"), v.productVersion);
        Add(rows, _T("Product version (binary)"), v.fixedProductVersion);
        Add(rows, _T("Internal name"), v.internalName);
        Add(rows, _T("Original file name"), v.originalFilename);
        Add(rows, _T("Copyright"), v.legalCopyright);
        Add(rows, _T("Language"), v.language);
        return rows;
    }

    std::vector<Property> SignatureProperties(const SignatureInfo& s)
    {
        std::vector<Property> rows;
        Add(rows, _T("Status"), s.valid ? CString(_T("Valid")) : CString(_T("Not valid")));
        Add(rows, _T("Result"), s.message);
        if (s.valid || !s.signer.empty())
        {
            Add(rows, _T("Signature type"), CString(s.catalogSigned ? _T("Catalog (listed in a system catalog file)") : _T("Embedded in the file")));
            Add(rows, _T("Signed by"), s.signer);
            Add(rows, _T("Issued by"), s.issuer);
            const ULONGLONG when = (ULONGLONG(s.verifiedAsOf.dwHighDateTime) << 32) | s.verifiedAsOf.dwLowDateTime;
            Add(rows, _T("Verified as of"), FileTimeText(when));
            Add(rows, _T("Catalog file"), s.catalogPath);
            Add(rows, _T("Revocation"), CString(s.revocationChecked ? _T("Checked against cached data") : _T("Not checked (no cached data, and this application stays offline)")));
        }
        CString code;
        code.Format(_T("0x%08X"), static_cast<unsigned>(s.status));
        Add(rows, _T("WinVerifyTrust result"), code);
        return rows;
    }

    CString ExportsCsv(const LibraryDetails& d)
    {
        CString text(_T("Ordinal,Name,Undecorated Name,RVA,Forwarded To or Defined In\r\n"));
        for (const ExportedFunction& e : d.exports)
        {
            CString ordinal, rva;
            if (e.ordinal)
                ordinal.Format(_T("%u"), e.ordinal);
            if (e.rva && !e.isForwarded)
                rva.Format(_T("0x%08X"), e.rva);
            text += ordinal + _T(",") + CsvField(fmt::Wide(ToWide(e.name))) + _T(",") + CsvField(fmt::Wide(ToWide(e.demangledName))) + _T(",") +
                    rva + _T(",") + CsvField(fmt::Wide(ToWide(e.forwardName))) + _T("\r\n");
        }
        return text;
    }

    CString ModuleStatusText(const DepModule& module)
    {
        switch (module.status)
        {
        case DepStatus::Ok:                return module.required ? CString(_T("Required")) : CString(_T("Optional (delay-loaded)"));
        case DepStatus::NotFound:          return CString(_T("Not found"));
        case DepStatus::WrongArchitecture: return CString(_T("Wrong architecture"));
        case DepStatus::Invalid:           return CString(_T("Can't be read"));
        default:                           return CString(_T("API set with no implementation"));
        }
    }

    CString SeverityText(DepSeverity severity)
    {
        return severity == DepSeverity::Error ? CString(_T("Error")) : severity == DepSeverity::Warning ? CString(_T("Warning")) : CString(_T("Info"));
    }

    CString FullReport(const LibraryDetails& d, const SignatureInfo& signature, const DependencyGraph* graph)
    {
        const auto section = [](CString& out, const CString& title) {
            out += _T("\r\n") + title + _T("\r\n") + CString(_T('-'), title.GetLength()) + _T("\r\n");
        };
        const auto properties = [](CString& out, const std::vector<Property>& rows) {
            for (const Property& row : rows)
                out += row.first + _T(": ") + row.second + _T("\r\n");
        };

        CString text;
        text.Format(_T("Library report for %s\r\n"), d.entry.FullPath().c_str());

        section(text, _T("Summary"));
        properties(text, SummaryProperties(d));
        section(text, _T("Version information"));
        properties(text, VersionProperties(d));
        section(text, _T("Digital signature"));
        properties(text, SignatureProperties(signature));

        CString heading;
        heading.Format(d.isArchive ? _T("Public symbols (%s)") : _T("Exported functions (%s)"), fmt::Count(d.exports.size()).GetString());
        section(text, heading);
        for (const ExportedFunction& e : d.exports)
        {
            CString line;
            if (e.ordinal)
                line.Format(_T("%6u  "), e.ordinal);
            line += ExportNameText(e);
            if (e.demangledName != e.name && !e.demangledName.empty())
                line += _T("  [") + fmt::Wide(ToWide(e.demangledName)) + _T("]");
            if (e.isForwarded)
                line += _T("  -> ") + fmt::Wide(ToWide(e.forwardName));
            else if (e.rva)
            {
                CString rva;
                rva.Format(_T("  RVA 0x%08X"), e.rva);
                line += rva;
            }
            else if (!e.forwardName.empty())
                line += _T("  in ") + fmt::Wide(ToWide(e.forwardName));
            text += line + _T("\r\n");
        }

        heading.Format(d.isArchive ? _T("Members (%s)") : _T("Dependencies (%s)"), fmt::Count(d.dependencies.size()).GetString());
        section(text, heading);
        for (const DllDependency& dep : d.dependencies)
        {
            text += fmt::Wide(ToWide(dep.dllName));
            if (dep.isDelayLoaded)
                text += _T("  (delay-loaded)");
            text += _T("\r\n");
            for (const ImportedFunction& f : dep.functions)
                text += _T("    ") + ImportNameText(f) + _T("\r\n");
        }

        if (graph)
        {
            section(text, _T("Dependency check"));
            text += fmt::Wide(graph->SummaryText()) + _T("\r\n");
            for (const DepProblem& p : graph->problems)
                text += SeverityText(p.severity) + _T(": ") + fmt::Wide(p.text) + _T("\r\n");

            heading.Format(_T("All modules in the chain (%s)"), fmt::Count(graph->modules.size()).GetString());
            section(text, heading);
            for (const DepModule& m : graph->modules)
            {
                text += fmt::Wide(m.name) + _T("  [") + ModuleStatusText(m) + _T("]");
                if (!m.path.empty())
                    text += _T("  ") + fmt::Wide(m.path);
                if (!m.foundVia.empty())
                    text += _T("  (") + fmt::Wide(m.foundVia) + _T(")");
                text += _T("\r\n");
            }
        }
        return text;
    }
}
