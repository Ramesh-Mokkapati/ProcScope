// ProcessReport.cpp
#include "pch.h"
#include "ProcessReport.h"
#include "Format.h"

namespace report
{
    CString CpuTimeText(ULONGLONG hundredNs)
    {
        const ULONGLONG ms = hundredNs / 10000;
        CString s;
        s.Format(_T("%I64u:%02I64u:%02I64u.%03I64u"), ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000);
        return s;
    }

    std::vector<Property> ProcessProperties(const ProcessRow& r, bool exited)
    {
        std::vector<Property> rows;
        const auto add = [&](LPCTSTR name, const CString& value) {
            if (!value.IsEmpty())
                rows.emplace_back(CString(name), value);
        };
        const auto addText = [&](LPCTSTR name, const std::wstring& value) { add(name, fmt::Wide(value)); };

        CString number;
        addText(_T("Process"), r.name);
        number.Format(_T("%u"), r.pid);
        add(_T("Process ID"), number);
        number.Format(_T("%u"), r.parentPid);
        add(_T("Parent process ID"), number);
        if (exited)
            add(_T("Status"), CString(_T("Exited. These are the last values seen.")));
        else if (r.accessDenied)
            add(_T("Status"), CString(_T("Running. Most details are denied: run as administrator to see them.")));
        else
            add(_T("Status"), CString(_T("Running")));
        addText(_T("Description"), r.description);
        addText(_T("Company"), r.company);
        addText(_T("Version"), r.version);
        addText(_T("Path"), r.path);
        addText(_T("Command line"), r.commandLine);
        addText(_T("Architecture"), r.architecture);
        addText(_T("User"), r.user);
        if (!r.user.empty())
            add(_T("Elevated"), CString(r.elevated ? _T("Yes") : _T("No")));
        if (!r.accessDenied && r.pid)
        {
            number.Format(_T("%u"), r.session);
            add(_T("Session"), number);
            addText(_T("Started"), FileTimeText(r.createTime));
            addText(_T("Priority"), PriorityClassName(r.priorityClass));
            add(_T("CPU now"), fmt::Percent(r.cpuPercent));
            add(_T("CPU time"), CpuTimeText(r.cpuTime));
            add(_T("Working set"), fmt::Bytes(double(r.workingSet)));
            add(_T("Private bytes"), fmt::Bytes(double(r.privateBytes)));
            add(_T("Disk and I/O read"), fmt::Bytes(r.readPerSec) + _T("/s"));
            add(_T("Disk and I/O write"), fmt::Bytes(r.writePerSec) + _T("/s"));
            add(_T("Handles"), fmt::Count(r.handles));
        }
        add(_T("Threads"), fmt::Count(r.threads));
        return rows;
    }

    CString ProcessReport(const ProcessRow& r, const ProcessDetails& d, const SignatureInfo* signature, const DependencyGraph* graph)
    {
        const auto section = [](CString& out, const CString& title) {
            out += _T("\r\n") + title + _T("\r\n") + CString(_T('-'), title.GetLength()) + _T("\r\n");
        };
        const auto properties = [](CString& out, const std::vector<Property>& rows) {
            for (const Property& row : rows)
                out += row.first + _T(": ") + row.second + _T("\r\n");
        };

        CString text;
        text.Format(_T("ProcScope report for %s (PID %u)\r\n"), r.name.c_str(), r.pid);

        section(text, _T("Process"));
        properties(text, ProcessProperties(r, false));

        if (d.hasFile)
        {
            section(text, _T("Version information"));
            properties(text, VersionProperties(d.file));
        }
        if (signature)
        {
            section(text, _T("Digital signature"));
            properties(text, SignatureProperties(*signature));
        }

        CString heading;
        heading.Format(_T("Loaded modules (%s)"), fmt::Count(d.modules.size()).GetString());
        section(text, heading);
        if (!d.modulesError.empty())
            text += fmt::Wide(d.modulesError) + _T("\r\n");
        for (const LoadedModule& m : d.modules)
        {
            CString line;
            line.Format(_T("0x%I64X  %s  %s  %s\r\n"), m.base, fmt::Bytes(double(m.size)).GetString(), m.version.c_str(), m.path.c_str());
            text += line;
        }

        heading.Format(_T("Threads (%s)"), fmt::Count(d.threads.size()).GetString());
        section(text, heading);
        for (const ThreadRow& t : d.threads)
        {
            CString line;
            line.Format(_T("%u  priority %d  %s\r\n"), t.id, t.priority, t.startText.c_str());
            text += line;
        }

        if (graph)
        {
            section(text, _T("Dependency check"));
            text += fmt::Wide(graph->SummaryText()) + _T("\r\n");
            for (const DepProblem& p : graph->problems)
                text += SeverityText(p.severity) + _T(": ") + fmt::Wide(p.text) + _T("\r\n");

            heading.Format(_T("All dependencies (%s)"), fmt::Count(graph->modules.size()).GetString());
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

    CString ProcessListCsv(const std::vector<ProcessRow>& rows)
    {
        CString text(_T("Name,PID,Parent PID,CPU %,Working Set (bytes),Private (bytes),Read (bytes/s),Write (bytes/s),Threads,Handles,Architecture,Version,Company,User,Path,Command Line\r\n"));
        for (const ProcessRow& r : rows)
        {
            CString numbers;
            numbers.Format(_T("%u,%u,%.1f,%I64u,%I64u,%.0f,%.0f,%u,%u"), r.pid, r.parentPid, r.cpuPercent, r.workingSet, r.privateBytes,
                           r.readPerSec, r.writePerSec, r.threads, r.handles);
            text += CsvField(fmt::Wide(r.name)) + _T(",") + numbers + _T(",") + CsvField(fmt::Wide(r.architecture)) + _T(",") +
                    CsvField(fmt::Wide(r.version)) + _T(",") + CsvField(fmt::Wide(r.company)) + _T(",") + CsvField(fmt::Wide(r.user)) + _T(",") +
                    CsvField(fmt::Wide(r.path)) + _T(",") + CsvField(fmt::Wide(r.commandLine)) + _T("\r\n");
        }
        return text;
    }
}
