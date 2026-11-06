// ProcScopeDlg.h : main window - the live process list, and details of the selected process.
#pragma once

#include "ProcessDetailsPane.h"
#include "ProcessLoader.h"
#include "VirtualList.h"
#include "resource.h"

class CProcScopeDlg : public CDialogEx
{
public:
    explicit CProcScopeDlg(const std::wstring& startSelection = std::wstring());

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* pMsg) override;
    void OnOK() override {}       // Enter must not close the window
    void OnCancel() override {}   // nor Escape

    afx_msg void OnClose();
    afx_msg void OnDestroy();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);

    afx_msg void OnPauseButton();
    afx_msg void OnFilterChanged();
    afx_msg void OnProcessChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnProcessDblClk(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg LRESULT OnSnapshotReady(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnProcessReady(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnSignatureReady(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDependenciesReady(WPARAM wParam, LPARAM lParam);

    afx_msg void OnFileSaveList();
    afx_msg void OnFileExit();
    afx_msg void OnViewRefresh();
    afx_msg void OnSpeed(UINT nID);
    afx_msg void OnHelpAbout();
    DECLARE_MESSAGE_MAP()

private:
    void LayoutControls();
    bool OnSplitter(CPoint point) const { return m_splitterRect.PtInRect(point) != FALSE; }

    const ProcessRow* SelectedProcess() const;
    void OnProcessSelected();
    void RequestDetails(const ProcessRow& row, bool force);
    void ApplyStartSelection();
    void UpdateSummary();
    void UpdateSpeedMenu();
    void UpdatePauseButton();

    void ShowProcessMenu(CPoint point);
    void GoToParent(DWORD parentPid);
    void SaveReport(const ProcessRow& row);

    HICON         m_hIcon = nullptr;
    HACCEL        m_hAccel = nullptr;
    CFont         m_font;
    std::wstring  m_startSelection;

    CStatic       m_summary;
    CButton       m_pause;
    CStatic       m_filterLabel;
    CEdit         m_filter;
    CVirtualList  m_list;
    CProcessDetailsPane m_details;
    CStatic       m_status;

    CSampleThread  m_sampler;
    CProcessLoader m_loader;

    std::shared_ptr<ProcessSnapshot> m_snapshot;
    std::vector<ProcessRow>          m_emptyRows;
    bool          m_paused = false;
    DWORD         m_intervalMs = 1000;

    UINT          m_requestId = 0;
    DWORD         m_detailPid = 0;
    ULONGLONG     m_detailCreate = 0;
    bool          m_hasDetail = false;

    double        m_detailFraction = 0.5;
    CRect         m_splitterRect;
    bool          m_dragging = false;
    int           m_contentTop = 0;
    int           m_contentBottom = 0;
};
