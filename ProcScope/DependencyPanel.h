// DependencyPanel.h : the Dependency Tree tab - the chain on the left, and on the right
// the functions the parent asks of the selected module.
#pragma once

#include "DependencyResolver.h"
#include "VirtualList.h"

class CDependencyPanel : public CWnd
{
public:
    using OpenFn = std::function<void(const std::wstring& path)>;

    BOOL CreatePanel(CWnd* pParent, UINT nID, CFont* pFont);
    void SetOpenHandler(OpenFn handler) { m_onOpen = std::move(handler); }

    void ShowMessage(const CString& message);
    void SetGraph(std::shared_ptr<const DependencyGraph> graph);
    void SetFilter(const CString& filter) { m_functions.SetFilter(filter); }

    // Expands down to one link in the chain and selects it.
    void GoTo(size_t importer, size_t edge);

protected:
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnItemExpanding(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnSelChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnRClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
    DECLARE_MESSAGE_MAP()

private:
    static constexpr size_t kNoEdge = size_t(-1);

    struct Node
    {
        size_t    module;
        size_t    importer;    // module holding the edge
        size_t    edge;        // kNoEdge for the root
        HTREEITEM item;
        Node*     parent;
        bool      loaded;
        bool      circular;
    };

    void      Layout(int cx, int cy);
    HTREEITEM AddNode(Node* parent, size_t importer, size_t edge);
    CString   NodeText(const Node& node) const;
    void      LoadChildren(Node& node);
    Node*     NodeOf(HTREEITEM item) const;
    void      ShowNode(const Node* node);
    void      ShowMenu(CPoint point);

    CFont*       m_pFont = nullptr;
    CStatic      m_info;
    CTreeCtrl    m_tree;
    CVirtualList m_functions;
    bool         m_created = false;

    std::shared_ptr<const DependencyGraph> m_graph;
    std::deque<Node> m_nodes;
    const DepEdge*   m_shownEdge = nullptr;
    OpenFn           m_onOpen;
};
