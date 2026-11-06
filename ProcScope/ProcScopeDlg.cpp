// ProcScopeDlg.cpp
#include "pch.h"
#include "ProcScope.h"
#include "ProcScopeDlg.h"
#include "Format.h"
#include "ProcessReport.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
    enum { IDC_PAUSE = 1100, IDC_FILTER, IDC_PROCESSES, IDC_DETAILS };
    enum { kSelectionTimer = 2, kSelectionDelayMs = 120, kFilterTimer = 3, kFilterDelayMs = 250 };

    enum { colName, colPid, colCpu, colWorkingSet, colPrivate, colRead, colWrite, colThreads, colHandles, colArch, colVersion, colCompany, colPath, colUser, colParent };

    // Context menu commands: returned by TrackPopupMenu, so they never reach the message map.
    enum
    {
        cmdSummary = 1, cmdModules, cmdThreads, cmdTree, cmdDependencies, cmdProblems, cmdVersion, cmdSignature,
        cmdReport, cmdParent, cmdLocation, cmdCopyPath, cmdCopyName, cmdCopyPid, cmdCopyCommand, cmdProperties
    };

    struct SpeedOption { UINT id; DWORD ms; };
    const SpeedOption kSpeeds[] = {
        { ID_SPEED_HIGH,    500 },
        { ID_SPEED_NORMAL,  1000 },
        { ID_SPEED_LOW,     2000 },
        { ID_SPEED_VERYLOW, 5000 },
    };

    class CAboutDlg : public CDialogEx
    {
    public:
        CAboutDlg() : CDialogEx(IDD_ABOUTBOX) {}
    };

    bool AskSavePath(CWnd* parent, const CString& defaultName, LPCTSTR extension, LPCTSTR filter, CString& path)
    {
        CFileDialog dialog(FALSE, extension, defaultName, OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST, filter, parent);
        if (dialog.DoModal() != IDOK)
            return false;
        path = dialog.GetPathName();
        return true;
    }
}

BEGIN_MESSAGE_MAP(CProcScopeDlg, CDialogEx)
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_TIMER()
    ON_WM_SETCURSOR()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_CAPTURECHANGED()
    ON_BN_CLICKED(IDC_PAUSE, &CProcScopeDlg::OnPauseButton)
    ON_EN_CHANGE(IDC_FILTER, &CProcScopeDlg::OnFilterChanged)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_PROCESSES, &CProcScopeDlg::OnProcessChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_PROCESSES, &CProcScopeDlg::OnProcessDblClk)
    ON_MESSAGE(WM_SNAPSHOT_READY, &CProcScopeDlg::OnSnapshotReady)
    ON_MESSAGE(WM_PROCESS_READY, &CProcScopeDlg::OnProcessReady)
    ON_MESSAGE(WM_SIGNATURE_READY, &CProcScopeDlg::OnSignatureReady)
    ON_MESSAGE(WM_DEPENDENCIES_READY, &CProcScopeDlg::OnDependenciesReady)
    ON_COMMAND(ID_FILE_SAVELIST, &CProcScopeDlg::OnFileSaveList)
    ON_COMMAND(ID_FILE_EXIT, &CProcScopeDlg::OnFileExit)
    ON_COMMAND(ID_VIEW_REFRESH, &CProcScopeDlg::OnViewRefresh)
    ON_COMMAND_RANGE(ID_SPEED_HIGH, ID_SPEED_VERYLOW, &CProcScopeDlg::OnSpeed)
    ON_COMMAND(ID_HELP_ABOUT, &CProcScopeDlg::OnHelpAbout)
END_MESSAGE_MAP()

CProcScopeDlg::CProcScopeDlg(const std::wstring& startSelection)
    : CDialogEx(IDD_PROCSCOPE_DIALOG), m_startSelection(startSelection)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

BOOL CProcScopeDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);
    m_hAccel = LoadAccelerators(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_ACCELERATORS));

    CString title;
    title.Format(_T("ProcScope - Process Monitor%s"), theApp.IsElevated() ? _T(" (Administrator)") : _T(""));
    SetWindowText(title);

    NONCLIENTMETRICS ncm = { sizeof(ncm) };
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    m_font.CreateFontIndirect(&ncm.lfMessageFont);
    SetFont(&m_font);

    const CRect none(0, 0, 10, 10);
    m_summary.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, none, this);
    m_pause.Create(_T("&Pause"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, none, this, IDC_PAUSE);
    m_filterLabel.Create(_T("Filter:"), WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX, none, this);
    m_filter.CreateEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T(""), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, none, this, IDC_FILTER);
    m_filter.SetCueBanner(_T("name, PID, company, path..."));
    m_list.CreateList(this, IDC_PROCESSES, false);
    m_details.CreatePane(this, IDC_DETAILS, &m_font);
    m_status.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS | SS_SUNKEN, none, this);

    for (CWnd* child = GetWindow(GW_CHILD); child; child = child->GetNextWindow())
        child->SetFont(&m_font, FALSE);

    const int R = LVCFMT_RIGHT;
    m_list.SetColumns({ { _T("Process"), 210 }, { _T("PID"), 60, true, R }, { _T("CPU %"), 60, true, R }, { _T("Working Set"), 85, true, R },
                        { _T("Private"), 85, true, R }, { _T("Read/s"), 75, true, R }, { _T("Write/s"), 75, true, R }, { _T("Threads"), 60, true, R },
                        { _T("Handles"), 60, true, R }, { _T("Arch"), 50 }, { _T("Version"), 110 }, { _T("Company"), 170 }, { _T("Path"), 420 },
                        { _T("User"), 180 }, { _T("Parent PID"), 75, true, R } });
    m_list.SetSource(
        [this](size_t row, int col) -> CString {
            const ProcessRow& p = m_snapshot->rows[row];
            CString s;
            switch (col)
            {
            case colName:    return fmt::Wide(p.name);
            case colPid:     s.Format(_T("%u"), p.pid); return s;
            case colCpu:     if (!p.accessDenied) s.Format(_T("%.1f"), p.cpuPercent); return s;
            case colWorkingSet: return p.accessDenied ? CString() : fmt::Bytes(double(p.workingSet));
            case colPrivate: return p.accessDenied ? CString() : fmt::Bytes(double(p.privateBytes));
            case colRead:    return p.accessDenied ? CString() : fmt::Bytes(p.readPerSec);
            case colWrite:   return p.accessDenied ? CString() : fmt::Bytes(p.writePerSec);
            case colThreads: return fmt::Count(p.threads);
            case colHandles: return p.accessDenied || !p.pid ? CString() : fmt::Count(p.handles);
            case colArch:    return fmt::Wide(p.architecture);
            case colVersion: return fmt::Wide(p.version);
            case colCompany: return p.accessDenied ? CString(_T("Access denied")) : fmt::Wide(p.company);
            case colPath:    return fmt::Wide(p.path);
            case colUser:    return fmt::Wide(p.user);
            default:         s.Format(_T("%u"), p.parentPid); return s;
            }
        },
        [this](size_t row, int col) -> double {
            const ProcessRow& p = m_snapshot->rows[row];
            switch (col)
            {
            case colPid:     return double(p.pid);
            case colCpu:     return p.cpuPercent;
            case colWorkingSet: return double(p.workingSet);
            case colPrivate: return double(p.privateBytes);
            case colRead:    return p.readPerSec;
            case colWrite:   return p.writePerSec;
            case colThreads: return double(p.threads);
            case colHandles: return double(p.handles);
            default:         return double(p.parentPid);
            }
        },
        [this](size_t row) { return m_snapshot->rows[row].accessDenied ? RGB(150, 150, 150) : COLORREF(CLR_DEFAULT); });
    m_list.SetContextMenuHandler([this](CPoint point) { ShowProcessMenu(point); });
    m_list.Sort(colCpu, FALSE);

    m_details.ShowPlaceholder(_T("Select a process to see its loaded modules, threads, dependencies, version and signature."));

    // Initial size: 1320 x 880 at 96 DPI, capped to the work area, centred.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(MonitorFromWindow(GetSafeHwnd(), MONITOR_DEFAULTTONEAREST), &mi);
    const CRect work(mi.rcWork);
    SetWindowPos(nullptr, 0, 0, min(fmt::Dpi(1320), work.Width() * 95 / 100), min(fmt::Dpi(880), work.Height() * 95 / 100),
                 SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow();
    LayoutControls();
    UpdateSpeedMenu();

    m_loader.Start(GetSafeHwnd());
    m_sampler.Start(GetSafeHwnd(), m_intervalMs);
    return TRUE;
}

BOOL CProcScopeDlg::PreTranslateMessage(MSG* pMsg)
{
    if (m_hAccel && pMsg->message >= WM_KEYFIRST && pMsg->message <= WM_KEYLAST &&
        ::TranslateAccelerator(GetSafeHwnd(), m_hAccel, pMsg))
        return TRUE;
    return CDialogEx::PreTranslateMessage(pMsg);
}

void CProcScopeDlg::OnClose()
{
    m_sampler.Stop();
    m_loader.Stop();
    EndDialog(IDCANCEL);
}

void CProcScopeDlg::OnDestroy()
{
    KillTimer(kSelectionTimer);
    KillTimer(kFilterTimer);
    m_sampler.Stop();
    m_loader.Stop();
    CDialogEx::OnDestroy();
}

// ---------------------------------------------------------------------------
// Layout

void CProcScopeDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    if (nType != SIZE_MINIMIZED)
        LayoutControls();
}

void CProcScopeDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    lpMMI->ptMinTrackSize.x = fmt::Dpi(860);
    lpMMI->ptMinTrackSize.y = fmt::Dpi(620);
    CDialogEx::OnGetMinMaxInfo(lpMMI);
}

void CProcScopeDlg::LayoutControls()
{
    if (!m_status.GetSafeHwnd())
        return;

    CRect rc;
    GetClientRect(&rc);
    const int cx = rc.Width(), cy = rc.Height();
    const int m = fmt::Dpi(8), gap = fmt::Dpi(6), rowH = fmt::Dpi(26), statusH = fmt::Dpi(22), splitter = fmt::Dpi(6);
    const int buttonH = rowH - fmt::Dpi(2);

    int y = m;
    const int pauseW = fmt::Dpi(80), filterW = fmt::Dpi(220), filterLabelW = fmt::Dpi(36);
    m_pause.MoveWindow(m, y, pauseW, buttonH);
    const int filterX = cx - m - filterW;
    m_filter.MoveWindow(filterX, y, filterW, buttonH);
    m_filterLabel.MoveWindow(filterX - gap - filterLabelW, y + fmt::Dpi(4), filterLabelW, buttonH - fmt::Dpi(6));
    const int summaryX = m + pauseW + gap * 2;
    m_summary.MoveWindow(summaryX, y + fmt::Dpi(4), max(filterX - gap * 2 - filterLabelW - summaryX, 0), buttonH - fmt::Dpi(6));
    y += rowH + fmt::Dpi(2);

    const int bottom = cy - statusH - fmt::Dpi(2);
    m_contentTop = y;
    m_contentBottom = bottom;
    const int contentH = max(bottom - y, 0);
    const int minDetails = fmt::Dpi(160), minUpper = fmt::Dpi(110);
    int detailsH = max(int(contentH * m_detailFraction), minDetails);
    detailsH = min(detailsH, max(contentH - minUpper - splitter, minDetails));
    const int upperH = max(contentH - detailsH - splitter, 0);

    m_list.MoveWindow(m, y, cx - 2 * m, upperH);
    m_splitterRect.SetRect(m, y + upperH, cx - m, y + upperH + splitter);
    m_details.MoveWindow(m, y + upperH + splitter, cx - 2 * m, detailsH);
    m_status.MoveWindow(0, cy - statusH, cx, statusH);
}

BOOL CProcScopeDlg::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
    if (nHitTest == HTCLIENT && pWnd == this)
    {
        CPoint point;
        GetCursorPos(&point);
        ScreenToClient(&point);
        if (m_dragging || OnSplitter(point))
        {
            ::SetCursor(::LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
        }
    }
    return CDialogEx::OnSetCursor(pWnd, nHitTest, message);
}

void CProcScopeDlg::OnLButtonDown(UINT nFlags, CPoint point)
{
    m_dragging = OnSplitter(point);
    if (m_dragging)
        SetCapture();
    else
        CDialogEx::OnLButtonDown(nFlags, point);
}

void CProcScopeDlg::OnMouseMove(UINT nFlags, CPoint point)
{
    if (m_dragging && GetCapture() == this)
    {
        const double height = max(m_contentBottom - m_contentTop, 1);
        m_detailFraction = min(max((m_contentBottom - point.y) / height, 0.15), 0.85);
        LayoutControls();
        return;
    }
    CDialogEx::OnMouseMove(nFlags, point);
}

void CProcScopeDlg::OnLButtonUp(UINT nFlags, CPoint point)
{
    if (m_dragging)
    {
        m_dragging = false;
        ReleaseCapture();
        return;
    }
    CDialogEx::OnLButtonUp(nFlags, point);
}

void CProcScopeDlg::OnCaptureChanged(CWnd* pWnd)
{
    m_dragging = false;
    CDialogEx::OnCaptureChanged(pWnd);
}

// ---------------------------------------------------------------------------
// Live list

LRESULT CProcScopeDlg::OnSnapshotReady(WPARAM, LPARAM)
{
    auto snapshot = m_sampler.Take();
    if (!snapshot)
        return 0;

    // Rows move between snapshots: the selection follows the process, not the row number.
    bool hadSelection = false;
    DWORD selectedPid = 0;
    ULONGLONG selectedCreate = 0;
    if (const ProcessRow* selected = SelectedProcess())
    {
        hadSelection = true;
        selectedPid = selected->pid;
        selectedCreate = selected->createTime;
    }

    m_snapshot = std::move(snapshot);

    std::vector<size_t> keep;
    const ProcessRow* liveDetail = nullptr;
    for (size_t r = 0; r < m_snapshot->rows.size(); ++r)
    {
        const ProcessRow& row = m_snapshot->rows[r];
        if (hadSelection && row.pid == selectedPid && row.createTime == selectedCreate)
            keep.push_back(r);
        if (m_hasDetail && row.pid == m_detailPid && row.createTime == m_detailCreate)
            liveDetail = &row;
    }
    m_list.Update(m_snapshot->rows.size(), keep);

    if (m_hasDetail)
        m_details.SetLive(liveDetail);
    if (!m_startSelection.empty())
        ApplyStartSelection();
    UpdateSummary();
    return 0;
}

void CProcScopeDlg::ApplyStartSelection()
{
    const std::wstring wanted = m_startSelection;
    m_startSelection.clear();

    const DWORD pid = wcstoul(wanted.c_str(), nullptr, 10);
    for (size_t r = 0; r < m_snapshot->rows.size(); ++r)
    {
        const ProcessRow& row = m_snapshot->rows[r];
        if ((pid && row.pid == pid) || (!pid && _wcsicmp(row.name.c_str(), wanted.c_str()) == 0))
        {
            m_list.SelectRow(r);
            OnProcessSelected();
            return;
        }
    }
}

void CProcScopeDlg::UpdateSummary()
{
    if (!m_snapshot)
        return;
    CString text;
    text.Format(_T("%s processes  |  CPU %.0f %%  |  Memory %s of %s  |  %s threads  |  %s handles"),
                fmt::Count(m_snapshot->rows.size()).GetString(), m_snapshot->cpuPercent,
                fmt::Bytes(double(m_snapshot->memoryUsed)).GetString(), fmt::Bytes(double(m_snapshot->memoryTotal)).GetString(),
                fmt::Count(m_snapshot->threads).GetString(), fmt::Count(m_snapshot->handles).GetString());
    CString old;
    m_summary.GetWindowText(old);
    if (old != text)
        m_summary.SetWindowText(text);

    size_t denied = 0;
    for (const ProcessRow& row : m_snapshot->rows)
        denied += row.accessDenied ? 1 : 0;

    CString refresh(_T("Paused"));
    if (!m_paused)
        refresh.Format(_T("Refresh every %.1f s"), m_intervalMs / 1000.0);
    const CString access = theApp.IsElevated() ? CString(_T("Running as administrator"))
                                               : fmt::Count(denied) + _T(" processes restricted: run as administrator for full details");
    CString status;
    status.Format(_T("  %s shown  |  %s  |  %s"), fmt::Count(m_list.VisibleCount()).GetString(), refresh.GetString(), access.GetString());
    m_status.GetWindowText(old);
    if (old != status)
        m_status.SetWindowText(status);
}

void CProcScopeDlg::OnPauseButton()
{
    m_paused = !m_paused;
    m_sampler.SetPaused(m_paused);
    UpdatePauseButton();
    UpdateSummary();
}

void CProcScopeDlg::UpdatePauseButton()
{
    m_pause.SetWindowText(m_paused ? _T("Res&ume") : _T("&Pause"));
}

void CProcScopeDlg::OnSpeed(UINT nID)
{
    for (const SpeedOption& s : kSpeeds)
        if (s.id == nID)
            m_intervalMs = s.ms;
    m_sampler.SetInterval(m_intervalMs);
    UpdateSpeedMenu();
    UpdateSummary();
}

void CProcScopeDlg::UpdateSpeedMenu()
{
    if (CMenu* menu = GetMenu())
        for (const SpeedOption& s : kSpeeds)
            if (s.ms == m_intervalMs)
                menu->CheckMenuRadioItem(ID_SPEED_HIGH, ID_SPEED_VERYLOW, s.id, MF_BYCOMMAND);
}

void CProcScopeDlg::OnFilterChanged()
{
    SetTimer(kFilterTimer, kFilterDelayMs, nullptr);
}

void CProcScopeDlg::OnTimer(UINT_PTR nIDEvent)
{
    switch (nIDEvent)
    {
    case kSelectionTimer:
        KillTimer(kSelectionTimer);
        OnProcessSelected();
        break;
    case kFilterTimer:
    {
        KillTimer(kFilterTimer);
        CString text;
        m_filter.GetWindowText(text);
        text.Trim();
        m_list.SetFilter(text);
        UpdateSummary();
        break;
    }
    default:
        CDialogEx::OnTimer(nIDEvent);
    }
}

void CProcScopeDlg::OnViewRefresh()
{
    // Reloads the selected process too: its modules and threads aren't sampled continuously.
    m_sampler.SampleNow();
    if (const ProcessRow* row = SelectedProcess())
        RequestDetails(*row, true);
}

// ---------------------------------------------------------------------------
// Selection and details

const ProcessRow* CProcScopeDlg::SelectedProcess() const
{
    size_t row = 0;
    if (!m_snapshot || !m_list.GetSelectedRow(row) || row >= m_snapshot->rows.size())
        return nullptr;
    return &m_snapshot->rows[row];
}

void CProcScopeDlg::OnProcessChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    if (reinterpret_cast<NMLISTVIEW*>(pNMHDR)->uChanged & LVIF_STATE)
        SetTimer(kSelectionTimer, kSelectionDelayMs, nullptr);   // settles first, so arrow keys don't load every row
}

void CProcScopeDlg::OnProcessSelected()
{
    if (const ProcessRow* row = SelectedProcess())
        RequestDetails(*row, false);
}

void CProcScopeDlg::RequestDetails(const ProcessRow& row, bool force)
{
    if (!force && m_hasDetail && row.pid == m_detailPid && row.createTime == m_detailCreate)
        return;
    m_hasDetail = true;
    m_detailPid = row.pid;
    m_detailCreate = row.createTime;
    m_details.ShowLoading(row);
    m_loader.Request(row.pid, row.createTime, row.path, ++m_requestId);
}

LRESULT CProcScopeDlg::OnProcessReady(WPARAM wParam, LPARAM)
{
    auto details = m_loader.TakeDetails();
    if (details && UINT(wParam) == m_requestId)
        m_details.SetDetails(std::move(details));
    return 0;
}

LRESULT CProcScopeDlg::OnSignatureReady(WPARAM wParam, LPARAM)
{
    auto signature = m_loader.TakeSignature();
    if (signature && UINT(wParam) == m_requestId)
        m_details.SetSignature(std::move(signature));
    return 0;
}

LRESULT CProcScopeDlg::OnDependenciesReady(WPARAM wParam, LPARAM)
{
    auto graph = m_loader.TakeDependencies();
    if (graph && UINT(wParam) == m_requestId)
        m_details.SetDependencies(std::move(graph));
    return 0;
}

void CProcScopeDlg::OnProcessDblClk(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    if (reinterpret_cast<NMITEMACTIVATE*>(pNMHDR)->iItem >= 0)
        m_details.ShowTab(CProcessDetailsPane::tabModules);
}

void CProcScopeDlg::ShowProcessMenu(CPoint point)
{
    const ProcessRow* selected = SelectedProcess();
    if (!selected)
        return;
    // Copied: a new snapshot can arrive while the menu is open.
    const ProcessRow row = *selected;
    const UINT hasPath = row.path.empty() ? MF_GRAYED : 0;

    CMenu menu;
    menu.CreatePopupMenu();
    menu.AppendMenu(MF_STRING, cmdSummary, _T("Show S&ummary"));
    menu.SetDefaultItem(cmdSummary);
    menu.AppendMenu(MF_STRING, cmdModules, _T("Show Loaded &Modules"));
    menu.AppendMenu(MF_STRING, cmdThreads, _T("Show T&hreads"));
    menu.AppendMenu(MF_STRING | hasPath, cmdTree, _T("Show Dependency Tr&ee"));
    menu.AppendMenu(MF_STRING | hasPath, cmdDependencies, _T("Show &All Dependencies"));
    menu.AppendMenu(MF_STRING | hasPath, cmdProblems, _T("Show Dependency Pro&blems"));
    menu.AppendMenu(MF_STRING | hasPath, cmdVersion, _T("Show &Version Information"));
    menu.AppendMenu(MF_STRING | hasPath, cmdSignature, _T("Show Di&gital Signature"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, cmdReport, _T("Save Full &Report..."));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, cmdParent, _T("Go to Paren&t Process"));
    menu.AppendMenu(MF_STRING | hasPath, cmdLocation, _T("Open File &Location"));
    menu.AppendMenu(MF_STRING | hasPath, cmdCopyPath, _T("&Copy Full Path"));
    menu.AppendMenu(MF_STRING, cmdCopyName, _T("Copy &Name"));
    menu.AppendMenu(MF_STRING, cmdCopyPid, _T("Copy P&ID"));
    menu.AppendMenu(MF_STRING | (row.commandLine.empty() ? MF_GRAYED : 0), cmdCopyCommand, _T("Copy Comman&d Line"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING | hasPath, cmdProperties, _T("Windows &Properties"));

    const UINT cmd = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, this);

    // Right-click is itself a way to look at a process, so the load shouldn't wait for the selection timer.
    if (cmd >= cmdSummary && cmd <= cmdSignature)
    {
        KillTimer(kSelectionTimer);
        RequestDetails(row, false);
    }

    switch (cmd)
    {
    case cmdSummary:      m_details.ShowTab(CProcessDetailsPane::tabSummary); break;
    case cmdModules:      m_details.ShowTab(CProcessDetailsPane::tabModules); break;
    case cmdThreads:      m_details.ShowTab(CProcessDetailsPane::tabThreads); break;
    case cmdTree:         m_details.ShowTab(CProcessDetailsPane::tabTree); break;
    case cmdDependencies: m_details.ShowTab(CProcessDetailsPane::tabDependencies); break;
    case cmdProblems:     m_details.ShowTab(CProcessDetailsPane::tabProblems); break;
    case cmdVersion:      m_details.ShowTab(CProcessDetailsPane::tabVersion); break;
    case cmdSignature:    m_details.ShowTab(CProcessDetailsPane::tabSignature); break;
    case cmdReport:       SaveReport(row); break;
    case cmdParent:       GoToParent(row.parentPid); break;
    case cmdLocation:     fmt::ShowInExplorer(GetSafeHwnd(), row.path, true); break;
    case cmdCopyPath:     fmt::CopyToClipboard(GetSafeHwnd(), fmt::Wide(row.path)); break;
    case cmdCopyName:     fmt::CopyToClipboard(GetSafeHwnd(), fmt::Wide(row.name)); break;
    case cmdCopyCommand:  fmt::CopyToClipboard(GetSafeHwnd(), fmt::Wide(row.commandLine)); break;
    case cmdCopyPid:
    {
        CString pid;
        pid.Format(_T("%u"), row.pid);
        fmt::CopyToClipboard(GetSafeHwnd(), pid);
        break;
    }
    case cmdProperties:
    {
        SHELLEXECUTEINFO info = { sizeof(info) };
        info.fMask = SEE_MASK_INVOKEIDLIST;
        info.hwnd = GetSafeHwnd();
        info.lpVerb = _T("properties");
        info.lpFile = row.path.c_str();
        info.nShow = SW_SHOW;
        ShellExecuteEx(&info);
        break;
    }
    }
}

void CProcScopeDlg::GoToParent(DWORD parentPid)
{
    if (m_snapshot)
        for (size_t r = 0; r < m_snapshot->rows.size(); ++r)
            if (m_snapshot->rows[r].pid == parentPid)
            {
                m_list.SelectRow(r);
                m_list.SetFocus();
                OnProcessSelected();
                return;
            }
    CString message;
    message.Format(_T("The parent process (PID %u) is no longer running, or the row is hidden by the filter."), parentPid);
    AfxMessageBox(message, MB_ICONINFORMATION);
}

void CProcScopeDlg::SaveReport(const ProcessRow& row)
{
    ProcessDetails details;
    SignatureInfo signature;
    std::shared_ptr<DependencyGraph> graph;
    {
        CWaitCursor wait;
        EnumerateModules(row.pid, details.modules, details.modulesError);
        EnumerateThreads(row.pid, details.modules, details.threads);
        if (!row.path.empty())
        {
            LoadDetails(row.path, details.file);
            details.hasFile = true;
            FileSignatureVerifier::Verify(row.path, signature);
            if (details.file.isPe)
            {
                DependencyResolver resolver;
                graph = resolver.Resolve(row.path);
            }
        }
    }
    CString target;
    if (!AskSavePath(this, fmt::Wide(row.name) + _T(".report.txt"), _T("txt"), _T("Text files (*.txt)|*.txt|All files (*.*)|*.*||"), target))
        return;
    const CString text = report::ProcessReport(row, details, details.hasFile ? &signature : nullptr, graph.get());
    if (!report::WriteUtf8File(target, text))
        AfxMessageBox(_T("The report could not be saved to that location."), MB_ICONERROR);
}

void CProcScopeDlg::OnFileSaveList()
{
    if (!m_snapshot)
        return;
    CString target;
    if (!AskSavePath(this, _T("processes.csv"), _T("csv"), _T("CSV files (*.csv)|*.csv|All files (*.*)|*.*||"), target))
        return;
    if (!report::WriteUtf8File(target, report::ProcessListCsv(m_snapshot->rows)))
        AfxMessageBox(_T("The list could not be saved to that location."), MB_ICONERROR);
}

void CProcScopeDlg::OnFileExit()
{
    PostMessage(WM_CLOSE);
}

void CProcScopeDlg::OnHelpAbout()
{
    CAboutDlg dialog;
    dialog.DoModal();
}
