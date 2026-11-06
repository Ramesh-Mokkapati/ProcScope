// Report.h : saves what the application shows to UTF-8 files.
#pragma once

#include "DependencyResolver.h"
#include "FileSignatureVerifier.h"
#include "LibraryAnalyzer.h"

namespace report
{
    // Writes UTF-8 with a BOM, so Excel and Notepad detect the encoding. False if the file can't be written.
    bool WriteUtf8File(const CString& path, const CString& text);

    CString CsvField(const CString& value);

    // Plain-text report: summary, version, signature, exports, dependencies and the dependency chain.
    // graph may be null, for a file with no chain to check.
    CString FullReport(const LibraryDetails& details, const SignatureInfo& signature, const DependencyGraph* graph);

    // Display text shared by the tabs and the report.
    CString ModuleStatusText(const DepModule& module);
    CString SeverityText(DepSeverity severity);

    // Exported functions (or public symbols, for a .lib) as CSV.
    CString ExportsCsv(const LibraryDetails& details);

    // Display name of an export with no name of its own.
    CString ExportNameText(const ExportedFunction& e);
    CString ImportNameText(const ImportedFunction& f);

    // Rows of the Summary, Version and Signature tabs; also used by FullReport.
    using Property = std::pair<CString, CString>;
    std::vector<Property> SummaryProperties(const LibraryDetails& details);
    std::vector<Property> VersionProperties(const LibraryDetails& details);
    std::vector<Property> SignatureProperties(const SignatureInfo& signature);
}
