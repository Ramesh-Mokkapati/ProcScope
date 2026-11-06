// ProcessDetailsPane.h : everything known about the selected process, one tab per kind of information.
#pragma once

#include "DependencyPanel.h"
#include "ProcessLoader.h"
#include "Report.h"
#include "VirtualList.h"

class CProcessDetailsPane : public CWnd
{
public:
    enum Tab { tabSummary, tabModules, tabThreads, tabTree, tabDependencies, tabProblems, tabVersion, tabSignature, tabCount };

    BOOL CreatePane(CWnd* pParent, UINT nID, CFont* pFont);

    void ShowPlaceholder(const CString& text);
    void ShowLoading(const ProcessRow& row);
    void SetDetails(std::shared_ptr<ProcessDetails> details);
    void SetSignature(std::shared_ptr<SignatureInfo> signature);
    void SetDependencies(std::shared_ptr<DependencyGraph> graph);

    // Called on every snapshot; null once the process has exited.
    void SetLive(const ProcessRow* row);

    void ShowTab(Tab tab);

protected:
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnFilterChanged();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    DECLARE_MESSAGE_MAP()

private:
    void Layout(int cx, int cy);
    void ReloadAll();
    void RebuildSummary();
    void UpdateTitle();
    void UpdateTabLabels();
    void ShowListMenu(CVirtualList& list, CPoint point, Tab tab);
    CString ModuleSource(const LoadedModule& module) const;
    bool IsLoaded(const DepModule& module) const;

    CFont*       m_pFont = nullptr;
    CFont        m_boldFont;
    CStatic      m_title;
    CStatic      m_filterLabel;
    CEdit        m_filter;
    CMFCTabCtrl  m_tabs;
    CVirtualList m_lists[tabCount];
    CDependencyPanel m_dependencyPanel;
    bool         m_created = false;

    bool         m_hasLive = false;
    bool         m_exited = false;
    ProcessRow   m_live;

    std::shared_ptr<ProcessDetails>  m_details;
    std::shared_ptr<SignatureInfo>   m_signature;
    std::shared_ptr<DependencyGraph> m_graph;
    std::vector<report::Property>    m_summaryRows;
    std::vector<report::Property>    m_versionRows;
    std::vector<report::Property>    m_signatureRows;
    std::unordered_set<std::wstring> m_loadedNames;     // lower-case file names
    std::unordered_set<std::wstring> m_requiredNames;
    std::unordered_set<std::wstring> m_chainNames;
};
