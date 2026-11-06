// ProcScope.h : application class
#pragma once

#include "resource.h"

class CProcScopeApp : public CWinAppEx
{
public:
    CProcScopeApp();

    BOOL InitInstance() override;
    bool IsElevated() const { return m_elevated; }

    DECLARE_MESSAGE_MAP()

private:
    static bool QueryElevation();
    static void EnableDebugPrivilege();
    bool m_elevated = false;
};

extern CProcScopeApp theApp;
