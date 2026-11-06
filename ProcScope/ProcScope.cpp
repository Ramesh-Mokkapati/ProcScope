// ProcScope.cpp : application entry point
#include "pch.h"
#include "ProcScope.h"
#include "ProcScopeDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CProcScopeApp, CWinAppEx)
END_MESSAGE_MAP()

CProcScopeApp theApp;

CProcScopeApp::CProcScopeApp()
{
    SetAppID(_T("ProcScope.ProcessMonitor.1.0"));
}

BOOL CProcScopeApp::InitInstance()
{
#ifdef _DEBUG
    // Report CRT leaks on exit (Output window when debugging).
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    CWinAppEx::InitInstance();

    // Needed by the file dialogs and the Windows Properties dialog.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    SetRegistryKey(_T("ProcScope"));

    CMFCVisualManagerOffice2007::SetStyle(CMFCVisualManagerOffice2007::Office2007_LunaBlue);
    CMFCVisualManager::SetDefaultManager(RUNTIME_CLASS(CMFCVisualManagerOffice2007));

    m_elevated = QueryElevation();
    if (m_elevated)
        EnableDebugPrivilege();   // lets it read services and other users' processes

    // ProcScope.exe [PID or process name]: starts with that process selected.
    std::wstring startSelection;
    if (__argc > 1 && __wargv[1])
        startSelection = __wargv[1];

    CProcScopeDlg dlg(startSelection);
    m_pMainWnd = &dlg;
    dlg.DoModal();

    CoUninitialize();

    // Dialog closed: return FALSE so the application exits instead of entering a message pump.
    return FALSE;
}

bool CProcScopeApp::QueryElevation()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD cb = 0;
    const bool elevated = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cb) && elevation.TokenIsElevated;
    CloseHandle(hToken);
    return elevated;
}

void CProcScopeApp::EnableDebugPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);
}
