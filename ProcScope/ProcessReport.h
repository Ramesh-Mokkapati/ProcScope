// ProcessReport.h : text for one process, shared by the Summary tab and saved reports.
#pragma once

#include "ProcessLoader.h"
#include "Report.h"

namespace report
{
    CString CpuTimeText(ULONGLONG hundredNs);

    std::vector<Property> ProcessProperties(const ProcessRow& row, bool exited);

    CString ProcessReport(const ProcessRow& row, const ProcessDetails& details, const SignatureInfo* signature, const DependencyGraph* graph);

    CString ProcessListCsv(const std::vector<ProcessRow>& rows);
}
