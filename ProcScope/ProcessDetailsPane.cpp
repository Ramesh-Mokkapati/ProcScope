// ProcessDetailsPane.cpp
#include "pch.h"
#include "ProcessDetailsPane.h"
#include "Format.h"
#include "ProcessReport.h"

namespace
{
    enum { IDC_FILTER = 5001, IDC_TABS, IDC_LIST_FIRST };
    enum { kFilterTimer = 1, kFilterDelayMs = 200 };
    enum { cmdCopy = 1, cmdCopyName, cmdCopyPath, cmdCopyAll, cmdSelectAll, cmdLocation, cmdShowInTree };

    const COLORREF kMuted = RGB(110, 110, 110);
    const COLORREF kBad = RGB(190, 0, 0);
    const COLORREF kWarn = RGB(190, 100, 0);
    const COLORREF kGood = RGB(0, 120, 40);
    const COLORREF kRunTime = RGB(70, 90, 150);

    std::wstring LowerName(const std::wstring& pathOrName)
    {
        std::wstring name = pathOrName;
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            name = name.substr(slash + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        return name;
    }

    CString Hex(ULONGLONG value)
    {
        CString s;
        s.Format(_T("0x%I64X"), value);
        return s;
    }
}

BEGIN_MESSAGE_MAP(CProcessDetailsPane, CWnd)
    ON_WM_SIZE()
    ON_WM_CTLCOLOR()
    ON_WM_TIMER()
    ON_EN_CHANGE(IDC_FILTER, &CProcessDetailsPane::OnFilterChanged)
END_MESSAGE_MAP()

BOOL CProcessDetailsPane::CreatePane(CWnd* pParent, UINT nID, CFont* pFont)
{
    m_pFont = pFont;
    LPCTSTR cls = AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), ::GetSysColorBrush(COLOR_BTNFACE), nullptr);
    if (!CWnd::Create(cls, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, CRect(0, 0, 10, 10), pParent, nID))
        return FALSE;

    if (!m_title.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, CRect(0, 0, 10, 10), this) ||
        !m_filterLabel.Create(_T("Find in details:"), WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX, CRect(0, 0, 10, 10), this) ||
        !m_filter.CreateEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T(""), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, CRect(0, 0, 10, 10), this, IDC_FILTER))
        return FALSE;
    m_filter.SetCueBanner(_T("module, function or thread"));

    if (!m_tabs.Create(CMFCTabCtrl::STYLE_3D_ONENOTE, CRect(0, 0, 10, 10), this, IDC_TABS, CMFCTabCtrl::LOCATION_TOP))
        return FALSE;
    m_tabs.EnableTabSwap(FALSE);
    m_tabs.AutoDestroyWindow(FALSE);   // the lists are members of this pane

    for (int i = 0; i < tabCount; ++i)
        if (i != tabTree && !m_lists[i].CreateList(&m_tabs, IDC_LIST_FIRST + i, true))
            return FALSE;
    if (!m_dependencyPanel.CreatePanel(&m_tabs, IDC_LIST_FIRST + tabTree, m_pFont))
        return FALSE;

    const int R = LVCFMT_RIGHT;
    const std::vector<CVirtualList::Column> properties = { { _T("Property"), 190 }, { _T("Value"), 760 } };
    m_lists[tabSummary].SetColumns(properties);
    m_lists[tabVersion].SetColumns(properties);
    m_lists[tabSignature].SetColumns(properties);
    m_lists[tabModules].SetColumns({ { _T("Module"), 230 }, { _T("How It Got Here"), 170 }, { _T("Base Address"), 135 }, { _T("Size"), 75, true, R },
                                     { _T("Version"), 110 }, { _T("Company"), 170 }, { _T("Full Path"), 420 } });
    m_lists[tabThreads].SetColumns({ { _T("Thread ID"), 75, true, R }, { _T("Priority"), 60, true, R }, { _T("CPU Time"), 95, true, R },
                                     { _T("Started"), 125, true }, { _T("Start Address"), 360 } });
    m_lists[tabDependencies].SetColumns({ { _T("Module"), 230 }, { _T("Status"), 150 }, { _T("Loaded Now"), 80 }, { _T("Found Via"), 120 }, { _T("Arch"), 55 },
                                          { _T("Version"), 110 }, { _T("Company"), 170 }, { _T("Full Path"), 420 } });
    m_lists[tabProblems].SetColumns({ { _T("Severity"), 75 }, { _T("Problem"), 900 } });

    const auto propertyText = [](const std::vector<report::Property>& rows) {
        return [&rows](size_t row, int col) { return col == 0 ? rows[row].first : rows[row].second; };
    };
    m_lists[tabSummary].SetSource(propertyText(m_summaryRows), nullptr, [this](size_t row) {
        const CString& name = m_summaryRows[row].first;
        if (name == _T("Problem") || name == _T("Status"))
            return m_exited || name == _T("Problem") ? kBad : COLORREF(CLR_DEFAULT);
        if (name == _T("Dependency check") && m_graph && m_graph->failure.empty())
            return m_graph->errors ? kBad : m_graph->warnings ? kWarn : kGood;
        return COLORREF(CLR_DEFAULT);
    });
    m_lists[tabVersion].SetSource(propertyText(m_versionRows));
    m_lists[tabSignature].SetSource(propertyText(m_signatureRows), nullptr, [this](size_t row) {
        if (m_signatureRows[row].first != _T("Status"))
            return COLORREF(CLR_DEFAULT);
        return !m_signature ? kMuted : m_signature->valid ? kGood : kBad;
    });

    m_lists[tabModules].SetSource(
        [this](size_t row, int col) -> CString {
            const LoadedModule& m = m_details->modules[row];
            switch (col)
            {
            case 0: return fmt::Wide(m.name);
            case 1: return ModuleSource(m);
            case 2: return Hex(m.base);
            case 3: return fmt::Bytes(double(m.size));
            case 4: return fmt::Wide(m.version);
            case 5: return fmt::Wide(m.company);
            default: return fmt::Wide(m.path);
            }
        },
        [this](size_t row, int) { return double(m_details->modules[row].size); },
        [this](size_t row) {
            return ModuleSource(m_details->modules[row]) == _T("Loaded at run time") ? kRunTime : COLORREF(CLR_DEFAULT);
        });

    m_lists[tabThreads].SetSource(
        [this](size_t row, int col) -> CString {
            const ThreadRow& t = m_details->threads[row];
            CString s;
            switch (col)
            {
            case 0: s.Format(_T("%u"), t.id); return s;
            case 1: s.Format(_T("%d"), t.priority); return s;
            case 2: return t.cpuTime ? report::CpuTimeText(t.cpuTime) : CString();
            case 3: return fmt::Wide(FileTimeText(t.created));
            default:
                if (!t.startText.empty())
                    return fmt::Wide(t.startText);
                return t.startAddress ? Hex(t.startAddress) : CString();
            }
        },
        [this](size_t row, int col) -> double {
            const ThreadRow& t = m_details->threads[row];
            switch (col)
            {
            case 0: return double(t.id);
            case 1: return double(t.priority);
            case 2: return double(t.cpuTime);
            default: return double(t.created);
            }
        });

    m_lists[tabDependencies].SetSource(
        [this](size_t row, int col) -> CString {
            const DepModule& m = m_graph->modules[row];
            switch (col)
            {
            case 0: return fmt::Wide(m.name);
            case 1: return report::ModuleStatusText(m);
            case 2: return !m_details || !m_details->modulesError.empty() ? CString() : IsLoaded(m) ? CString(_T("Yes")) : CString(_T("No"));
            case 3: return fmt::Wide(m.foundVia);
            case 4: return m.machine ? fmt::Wide(MachineName(m.machine)) : CString();
            case 5: return fmt::Wide(m.version);
            case 6: return fmt::Wide(m.company);
            default: return !m.path.empty() ? fmt::Wide(m.path) : fmt::Wide(m.detail);
            }
        },
        nullptr,
        [this](size_t row) {
            const DepModule& m = m_graph->modules[row];
            if (m.status == DepStatus::Ok)
                return COLORREF(CLR_DEFAULT);
            if (m.status == DepStatus::ApiSetNoHost)
                return kMuted;
            return m.required ? kBad : kWarn;
        });

    m_lists[tabProblems].SetSource(
        [this](size_t row, int col) -> CString {
            const DepProblem& p = m_graph->problems[row];
            return col == 0 ? report::SeverityText(p.severity) : fmt::Wide(p.text);
        },
        nullptr,
        [this](size_t row) {
            const DepSeverity s = m_graph->problems[row].severity;
            return s == DepSeverity::Error ? kBad : s == DepSeverity::Warning ? kWarn : kMuted;
        });

    static const LPCTSTR kTitles[tabCount] = { _T("Summary"), _T("Loaded Modules"), _T("Threads"), _T("Dependency Tree"),
                                               _T("All Dependencies"), _T("Problems"), _T("Version"), _T("Digital Signature") };
    for (int i = 0; i < tabCount; ++i)
    {
        if (i == tabTree)
        {
            m_tabs.AddTab(&m_dependencyPanel, kTitles[i], UINT(-1), FALSE);
            continue;
        }
        CVirtualList& list = m_lists[i];
        const Tab tab = Tab(i);
        list.SetContextMenuHandler([this, &list, tab](CPoint point) { ShowListMenu(list, point, tab); });
        list.SetFont(m_pFont, FALSE);
        m_tabs.AddTab(&list, kTitles[i], UINT(-1), FALSE);
    }
    m_lists[tabModules].Sort(0, TRUE);
    m_lists[tabThreads].Sort(2, FALSE);
    m_lists[tabDependencies].Sort(0, TRUE);
    m_tabs.SetActiveTab(tabSummary);

    m_filterLabel.SetFont(m_pFont, FALSE);
    m_filter.SetFont(m_pFont, FALSE);
    m_tabs.SetFont(m_pFont);

    LOGFONT lf = {};
    m_pFont->GetLogFont(&lf);
    lf.lfWeight = FW_BOLD;
    m_boldFont.CreateFontIndirect(&lf);
    m_title.SetFont(&m_boldFont, FALSE);

    m_created = true;
    CRect rc;
    GetClientRect(&rc);
    Layout(rc.Width(), rc.Height());
    return TRUE;
}

void CProcessDetailsPane::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);
    if (m_created && cx > 0 && cy > 0)
        Layout(cx, cy);
}

HBRUSH CProcessDetailsPane::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HBRUSH hbr = CWnd::OnCtlColor(pDC, pWnd, nCtlColor);
    if (nCtlColor == CTLCOLOR_STATIC)
    {
        pDC->SetBkColor(::GetSysColor(COLOR_BTNFACE));
        return ::GetSysColorBrush(COLOR_BTNFACE);
    }
    return hbr;
}

void CProcessDetailsPane::Layout(int cx, int cy)
{
    const int rowH = fmt::Dpi(24);
    const int filterW = fmt::Dpi(220), labelW = fmt::Dpi(96), gap = fmt::Dpi(6);
    const int filterX = max(cx - filterW, 0);
    const int labelX = max(filterX - gap - labelW, 0);
    m_filter.MoveWindow(filterX, 0, min(filterW, cx), rowH - fmt::Dpi(2));
    m_filterLabel.MoveWindow(labelX, fmt::Dpi(3), labelW, rowH - fmt::Dpi(6));
    m_title.MoveWindow(0, fmt::Dpi(3), max(labelX - gap, 0), rowH - fmt::Dpi(6));
    m_tabs.MoveWindow(0, rowH, cx, max(cy - rowH, 0));
}

CString CProcessDetailsPane::ModuleSource(const LoadedModule& module) const
{
    const std::wstring name = LowerName(module.path.empty() ? module.name : module.path);
    if (m_details && !m_details->path.empty() && name == LowerName(m_details->path))
        return CString(_T("Process image"));
    if (!m_graph)
        return CString();
    if (m_requiredNames.count(name))
        return CString(_T("Needed at startup"));
    if (m_chainNames.count(name))
        return CString(_T("Optional dependency"));
    return CString(_T("Loaded at run time"));
}

bool CProcessDetailsPane::IsLoaded(const DepModule& module) const
{
    return m_loadedNames.count(LowerName(module.path.empty() ? module.name : module.path)) != 0;
}

void CProcessDetailsPane::ShowPlaceholder(const CString& text)
{
    m_details.reset();
    m_hasLive = false;
    m_exited = false;
    ReloadAll();
    m_title.SetWindowText(text);
}

void CProcessDetailsPane::ShowLoading(const ProcessRow& row)
{
    // The previous process stays on screen until the new one arrives, which avoids a flicker per row.
    m_live = row;
    m_hasLive = true;
    m_exited = false;
    CString title;
    title.Format(_T("%s  (PID %u)  (loading...)"), row.name.c_str(), row.pid);
    m_title.SetWindowText(title);
}

void CProcessDetailsPane::UpdateTitle()
{
    if (!m_hasLive)
        return;
    CString title;
    title.Format(_T("%s  (PID %u)"), m_live.name.c_str(), m_live.pid);
    if (m_exited)
        title += _T("  (exited)");
    if (!m_live.path.empty())
        title += _T("   ") + fmt::Wide(m_live.path);
    CString old;
    m_title.GetWindowText(old);
    if (old != title)
        m_title.SetWindowText(title);
}

void CProcessDetailsPane::SetDetails(std::shared_ptr<ProcessDetails> details)
{
    m_details = std::move(details);
    ReloadAll();
    UpdateTitle();
}

void CProcessDetailsPane::SetSignature(std::shared_ptr<SignatureInfo> signature)
{
    m_signature = std::move(signature);
    m_signatureRows = report::SignatureProperties(*m_signature);
    m_lists[tabSignature].Reset(m_signatureRows.size());
}

void CProcessDetailsPane::SetDependencies(std::shared_ptr<DependencyGraph> graph)
{
    m_graph = std::move(graph);
    m_requiredNames.clear();
    m_chainNames.clear();
    if (m_graph)
        for (const DepModule& m : m_graph->modules)
        {
            const std::wstring name = LowerName(m.path.empty() ? m.name : m.path);
            m_chainNames.insert(name);
            if (m.required)
                m_requiredNames.insert(name);
        }

    m_lists[tabDependencies].Reset(m_graph ? m_graph->modules.size() : 0);
    m_lists[tabProblems].Reset(m_graph ? m_graph->problems.size() : 0);
    m_lists[tabModules].Invalidate(FALSE);
    m_dependencyPanel.SetGraph(m_graph);
    RebuildSummary();
    UpdateTabLabels();
}

void CProcessDetailsPane::SetLive(const ProcessRow* row)
{
    if (!m_hasLive)
        return;
    if (row)
    {
        m_live = *row;
        m_exited = false;
    }
    else
        m_exited = true;
    UpdateTitle();
    if (m_details)
        RebuildSummary();
}

void CProcessDetailsPane::RebuildSummary()
{
    const size_t before = m_summaryRows.size();
    m_summaryRows.clear();
    if (m_hasLive && m_details)
    {
        m_summaryRows = report::ProcessProperties(m_live, m_exited);
        if (!m_details->modulesError.empty())
            m_summaryRows.emplace_back(CString(_T("Loaded modules")), fmt::Wide(m_details->modulesError));
        else
        {
            size_t atRunTime = 0;
            for (const LoadedModule& m : m_details->modules)
                atRunTime += m_graph && ModuleSource(m) == _T("Loaded at run time") ? 1 : 0;
            CString modules = fmt::Count(m_details->modules.size());
            if (m_graph)
                modules += _T("  (") + fmt::Count(atRunTime) + _T(" loaded at run time)");
            m_summaryRows.emplace_back(CString(_T("Loaded modules")), modules);
        }
        if (m_details->hasFile && m_details->file.isPe)
            m_summaryRows.emplace_back(CString(_T("Dependency check")),
                                       m_graph ? fmt::Wide(m_graph->SummaryText()) : CString(_T("Following the chain...")));
        if (m_details->hasFile)
            m_summaryRows.emplace_back(CString(_T("Image format")), m_details->file.isPe
                ? CString(m_details->file.headers.is64 ? _T("PE32+ (64-bit)") : _T("PE32 (32-bit)")) : fmt::Wide(m_details->file.entry.error));
    }
    if (before != m_summaryRows.size())
        m_lists[tabSummary].Reset(m_summaryRows.size());
    else
        m_lists[tabSummary].Invalidate(FALSE);
}

void CProcessDetailsPane::ReloadAll()
{
    // The rows read these, so the lists are emptied before they go.
    m_lists[tabDependencies].Reset(0);
    m_lists[tabProblems].Reset(0);
    m_graph.reset();
    m_signature.reset();
    m_requiredNames.clear();
    m_chainNames.clear();
    m_loadedNames.clear();
    m_versionRows.clear();
    m_signatureRows.clear();

    const bool hasFile = m_details && m_details->hasFile;
    if (m_details)
        for (const LoadedModule& m : m_details->modules)
            m_loadedNames.insert(LowerName(m.path.empty() ? m.name : m.path));

    m_dependencyPanel.ShowMessage(hasFile && m_details->file.isPe ? CString(_T("Following the dependency chain..."))
                                  : m_details ? CString(_T("This process's file can't be read, so its dependency chain isn't available. Run as administrator to try again."))
                                              : CString());
    if (hasFile)
    {
        m_versionRows = report::VersionProperties(m_details->file);
        m_signatureRows.emplace_back(CString(_T("Status")), CString(_T("Verifying...")));
    }
    else if (m_details)
    {
        m_versionRows.emplace_back(CString(_T("Version information")), CString(_T("Not available: this process's file can't be identified.")));
        m_signatureRows.emplace_back(CString(_T("Status")), CString(_T("Not available: this process's file can't be identified.")));
    }

    m_lists[tabModules].Reset(m_details ? m_details->modules.size() : 0);
    m_lists[tabThreads].Reset(m_details ? m_details->threads.size() : 0);
    m_lists[tabVersion].Reset(m_versionRows.size());
    m_lists[tabSignature].Reset(m_signatureRows.size());
    m_summaryRows.clear();
    m_lists[tabSummary].Reset(0);
    RebuildSummary();
    UpdateTabLabels();
}

void CProcessDetailsPane::UpdateTabLabels()
{
    const auto label = [](LPCTSTR name, size_t count, bool show) {
        CString text(name);
        if (show)
            text += _T(" (") + fmt::Count(count) + _T(")");
        return text;
    };
    const bool modulesKnown = m_details && m_details->modulesError.empty();
    m_tabs.SetTabLabel(tabModules, label(_T("Loaded Modules"), m_details ? m_details->modules.size() : 0, modulesKnown));
    m_tabs.SetTabLabel(tabThreads, label(_T("Threads"), m_details ? m_details->threads.size() : 0, m_details != nullptr));
    m_tabs.SetTabLabel(tabDependencies, label(_T("All Dependencies"), m_graph ? m_graph->modules.size() : 0, m_graph != nullptr));
    m_tabs.SetTabLabel(tabProblems, label(_T("Problems"), m_graph ? m_graph->errors + m_graph->warnings : 0, m_graph != nullptr));
    m_tabs.RecalcLayout();
    m_tabs.Invalidate(FALSE);
}

void CProcessDetailsPane::ShowTab(Tab tab)
{
    m_tabs.SetActiveTab(tab);
    if (tab == tabTree)
        m_dependencyPanel.SetFocus();
    else
        m_lists[tab].SetFocus();
}

void CProcessDetailsPane::OnFilterChanged()
{
    SetTimer(kFilterTimer, kFilterDelayMs, nullptr);
}

void CProcessDetailsPane::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent != kFilterTimer)
    {
        CWnd::OnTimer(nIDEvent);
        return;
    }
    KillTimer(kFilterTimer);
    CString text;
    m_filter.GetWindowText(text);
    text.Trim();
    // The property tabs are short and always shown in full.
    m_lists[tabModules].SetFilter(text);
    m_lists[tabThreads].SetFilter(text);
    m_lists[tabDependencies].SetFilter(text);
    m_lists[tabProblems].SetFilter(text);
    m_dependencyPanel.SetFilter(text);
}

void CProcessDetailsPane::ShowListMenu(CVirtualList& list, CPoint point, Tab tab)
{
    size_t row = 0;
    const bool hasRow = list.GetSelectedRow(row);
    const UINT rowFlag = hasRow ? 0 : MF_GRAYED;

    std::wstring path;
    if (hasRow && tab == tabModules)
        path = m_details->modules[row].path;
    else if (hasRow && tab == tabDependencies)
        path = m_graph->modules[row].path;

    CMenu menu;
    menu.CreatePopupMenu();
    if (tab == tabProblems)
    {
        menu.AppendMenu(MF_STRING | rowFlag, cmdShowInTree, _T("Show in Dependency &Tree"));
        menu.AppendMenu(MF_SEPARATOR);
    }
    if (tab == tabModules || tab == tabDependencies)
    {
        menu.AppendMenu(MF_STRING | (path.empty() ? MF_GRAYED : 0), cmdLocation, _T("Open File &Location"));
        menu.AppendMenu(MF_STRING | rowFlag, cmdCopyName, _T("Copy &Name"));
        menu.AppendMenu(MF_STRING | (path.empty() ? MF_GRAYED : 0), cmdCopyPath, _T("Copy Full &Path"));
        menu.AppendMenu(MF_SEPARATOR);
    }
    menu.AppendMenu(MF_STRING | rowFlag, cmdCopy, _T("&Copy Selected Rows\tCtrl+C"));
    menu.AppendMenu(MF_STRING, cmdCopyAll, _T("Copy &All Visible Rows"));
    menu.AppendMenu(MF_STRING, cmdSelectAll, _T("&Select All\tCtrl+A"));

    switch (menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, this))
    {
    case cmdCopy:
        fmt::CopyToClipboard(GetSafeHwnd(), list.SelectedRowsText());
        break;
    case cmdCopyName:
        fmt::CopyToClipboard(GetSafeHwnd(), list.CellText(row, 0));
        break;
    case cmdCopyPath:
        fmt::CopyToClipboard(GetSafeHwnd(), fmt::Wide(path));
        break;
    case cmdCopyAll:
        fmt::CopyToClipboard(GetSafeHwnd(), list.AllRowsText());
        break;
    case cmdSelectAll:
        list.SelectAll();
        break;
    case cmdLocation:
        fmt::ShowInExplorer(GetSafeHwnd(), path, true);
        break;
    case cmdShowInTree:
    {
        const DepProblem& problem = m_graph->problems[row];
        ShowTab(tabTree);
        m_dependencyPanel.GoTo(problem.importer, problem.edge);
        break;
    }
    }
}
