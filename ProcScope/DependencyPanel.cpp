// DependencyPanel.cpp
#include "pch.h"
#include "DependencyPanel.h"
#include "Format.h"
#include "LibraryAnalyzer.h"
#include "PeImage.h"
#include "Report.h"

namespace
{
    enum { IDC_DEP_TREE = 5101, IDC_DEP_FUNCTIONS };
    enum { cmdOpen = 1, cmdLocation, cmdCopyName, cmdCopyPath };

    const COLORREF kError = RGB(190, 0, 0);
    const COLORREF kWarning = RGB(190, 100, 0);
    const COLORREF kIndirect = RGB(70, 90, 150);
    const COLORREF kMuted = RGB(120, 120, 120);
}

BEGIN_MESSAGE_MAP(CDependencyPanel, CWnd)
    ON_WM_SIZE()
    ON_WM_CTLCOLOR()
    ON_WM_CONTEXTMENU()
    ON_NOTIFY(TVN_ITEMEXPANDING, IDC_DEP_TREE, &CDependencyPanel::OnItemExpanding)
    ON_NOTIFY(TVN_SELCHANGED, IDC_DEP_TREE, &CDependencyPanel::OnSelChanged)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_DEP_TREE, &CDependencyPanel::OnCustomDraw)
    ON_NOTIFY(NM_RCLICK, IDC_DEP_TREE, &CDependencyPanel::OnRClick)
END_MESSAGE_MAP()

BOOL CDependencyPanel::CreatePanel(CWnd* pParent, UINT nID, CFont* pFont)
{
    m_pFont = pFont;
    LPCTSTR cls = AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), ::GetSysColorBrush(COLOR_BTNFACE), nullptr);
    if (!CWnd::Create(cls, nullptr, WS_CHILD | WS_CLIPCHILDREN, CRect(0, 0, 10, 10), pParent, nID))
        return FALSE;

    if (!m_info.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, CRect(0, 0, 10, 10), this) ||
        !m_tree.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                       CRect(0, 0, 10, 10), this, IDC_DEP_TREE) ||
        !m_functions.CreateList(this, IDC_DEP_FUNCTIONS, true))
        return FALSE;

    m_info.SetFont(m_pFont, FALSE);
    m_tree.SetFont(m_pFont, FALSE);
    m_functions.SetFont(m_pFont, FALSE);

    m_functions.SetColumns({ { _T("Function Imported from This Module"), 320 }, { _T("Ordinal"), 65, true, LVCFMT_RIGHT }, { _T("Result"), 300 } });
    m_functions.SetSource(
        [this](size_t row, int col) -> CString {
            const DepFunction& f = m_shownEdge->functions[row];
            CString s;
            switch (col)
            {
            case 0:
                if (f.byOrdinal)
                    s.Format(_T("(by ordinal %u)"), f.ordinal);
                else
                    s = fmt::Wide(ToWide(f.name));
                return s;
            case 1:
                if (f.byOrdinal)
                    s.Format(_T("%u"), f.ordinal);
                return s;
            default:
                if (!f.checked)
                    return CString(_T("Not checked: the module isn't available"));
                if (!f.found)
                    return CString(_T("Missing: not exported by this module"));
                if (!f.forwardedTo.empty())
                    return _T("Forwarded to ") + fmt::Wide(ToWide(f.forwardedTo));
                return CString(_T("Found"));
            }
        },
        [this](size_t row, int) { return double(m_shownEdge->functions[row].ordinal); },
        [this](size_t row) {
            const DepFunction& f = m_shownEdge->functions[row];
            if (!f.checked)
                return kMuted;
            if (!f.found)
                return m_shownEdge->required ? kError : kWarning;
            return !f.forwardedTo.empty() ? kIndirect : COLORREF(CLR_DEFAULT);
        });
    m_functions.SetContextMenuHandler([this](CPoint point) {
        CMenu menu;
        menu.CreatePopupMenu();
        menu.AppendMenu(MF_STRING, 1, _T("&Copy Selected Rows\tCtrl+C"));
        menu.AppendMenu(MF_STRING, 2, _T("Copy &All Visible Rows"));
        const UINT cmd = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, this);
        if (cmd == 1)
            fmt::CopyToClipboard(GetSafeHwnd(), m_functions.SelectedRowsText());
        else if (cmd == 2)
            fmt::CopyToClipboard(GetSafeHwnd(), m_functions.AllRowsText());
    });
    m_functions.Sort(0, TRUE);

    m_created = true;
    CRect rc;
    GetClientRect(&rc);
    Layout(rc.Width(), rc.Height());
    return TRUE;
}

void CDependencyPanel::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);
    if (m_created && cx > 0 && cy > 0)
        Layout(cx, cy);
}

HBRUSH CDependencyPanel::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HBRUSH hbr = CWnd::OnCtlColor(pDC, pWnd, nCtlColor);
    if (nCtlColor == CTLCOLOR_STATIC)
    {
        pDC->SetBkColor(::GetSysColor(COLOR_BTNFACE));
        return ::GetSysColorBrush(COLOR_BTNFACE);
    }
    return hbr;
}

void CDependencyPanel::Layout(int cx, int cy)
{
    const int infoH = fmt::Dpi(22), gap = fmt::Dpi(6);
    m_info.MoveWindow(fmt::Dpi(4), fmt::Dpi(3), max(cx - fmt::Dpi(8), 0), infoH - fmt::Dpi(4));
    const int treeW = cx * 46 / 100;
    m_tree.MoveWindow(0, infoH, treeW, max(cy - infoH, 0));
    m_functions.MoveWindow(treeW + gap, infoH, max(cx - treeW - gap, 0), max(cy - infoH, 0));
}

void CDependencyPanel::ShowMessage(const CString& message)
{
    m_shownEdge = nullptr;
    m_functions.Reset(0);
    m_tree.DeleteAllItems();
    m_nodes.clear();
    m_graph.reset();
    m_info.SetWindowText(message);
}

CString CDependencyPanel::NodeText(const Node& node) const
{
    const DepModule& module = m_graph->modules[node.module];
    if (node.edge == kNoEdge)
        return fmt::Wide(module.name);

    const DepEdge& edge = m_graph->modules[node.importer].edges[node.edge];
    CString text = fmt::Wide(ToWide(edge.importedAs));
    if (edge.viaApiSet && module.status != DepStatus::ApiSetNoHost)
        text += _T("  \x2192  ") + fmt::Wide(module.name);

    if (edge.kind == DepKind::Delay)
        text += _T("  [delay-loaded]");
    else if (edge.kind == DepKind::Forward)
        text += _T("  [forwarded]");

    if (module.status != DepStatus::Ok)
        text += _T("  (") + report::ModuleStatusText(module) + _T(")");
    else if (edge.missingFunctions)
    {
        CString missing;
        missing.Format(_T("  (%s missing %s)"), fmt::Count(edge.missingFunctions).GetString(), edge.missingFunctions == 1 ? _T("function") : _T("functions"));
        text += missing;
    }
    if (node.circular)
        text += _T("  (circular)");
    return text;
}

HTREEITEM CDependencyPanel::AddNode(Node* parent, size_t importer, size_t edge)
{
    Node node = {};
    node.importer = importer;
    node.edge = edge;
    node.module = edge == kNoEdge ? 0 : m_graph->modules[importer].edges[edge].target;
    node.parent = parent;

    // A chain that leads back to a module already above it ends there.
    for (const Node* up = parent; up; up = up->parent)
        if (up->module == node.module)
            node.circular = true;

    m_nodes.push_back(node);
    Node& stored = m_nodes.back();

    const CString text = NodeText(stored);
    TVINSERTSTRUCT insert = {};
    insert.hParent = parent ? parent->item : TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
    insert.item.pszText = const_cast<LPTSTR>(text.GetString());
    insert.item.cChildren = !stored.circular && !m_graph->modules[stored.module].edges.empty() ? 1 : 0;
    insert.item.lParam = reinterpret_cast<LPARAM>(&stored);
    stored.item = m_tree.InsertItem(&insert);
    return stored.item;
}

void CDependencyPanel::LoadChildren(Node& node)
{
    if (node.loaded || node.circular)
        return;
    node.loaded = true;
    const size_t count = m_graph->modules[node.module].edges.size();
    for (size_t e = 0; e < count; ++e)
        AddNode(&node, node.module, e);
}

CDependencyPanel::Node* CDependencyPanel::NodeOf(HTREEITEM item) const
{
    return item ? reinterpret_cast<Node*>(m_tree.GetItemData(item)) : nullptr;
}

void CDependencyPanel::SetGraph(std::shared_ptr<const DependencyGraph> graph)
{
    ShowMessage(CString());
    m_graph = std::move(graph);
    if (!m_graph)
        return;
    if (!m_graph->failure.empty() || m_graph->modules.empty())
    {
        m_info.SetWindowText(fmt::Wide(m_graph->failure));
        return;
    }

    m_tree.SetRedraw(FALSE);
    const HTREEITEM root = AddNode(nullptr, 0, kNoEdge);
    LoadChildren(*NodeOf(root));
    m_tree.Expand(root, TVE_EXPAND);
    m_tree.SelectItem(root);
    m_tree.SetRedraw(TRUE);
}

void CDependencyPanel::OnItemExpanding(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    const NMTREEVIEW* nm = reinterpret_cast<NMTREEVIEW*>(pNMHDR);
    if (nm->action == TVE_EXPAND)
        if (Node* node = NodeOf(nm->itemNew.hItem))
            LoadChildren(*node);
}

void CDependencyPanel::ShowNode(const Node* node)
{
    if (!node || !m_graph)
    {
        m_shownEdge = nullptr;
        m_functions.Reset(0);
        return;
    }
    const DepModule& module = m_graph->modules[node->module];

    CString info;
    if (!module.path.empty())
    {
        info = fmt::Wide(module.path);
        if (!module.foundVia.empty())
            info += _T("   |   Found via: ") + fmt::Wide(module.foundVia);
        if (module.machine)
            info += _T("   |   ") + fmt::Wide(MachineName(module.machine));
        if (!module.version.empty())
            info += _T("   |   ") + fmt::Wide(module.version);
    }
    if (!module.detail.empty())
    {
        if (!info.IsEmpty())
            info += _T("   |   ");
        info += fmt::Wide(module.detail);
    }
    m_info.SetWindowText(info);

    m_shownEdge = node->edge == kNoEdge ? nullptr : &m_graph->modules[node->importer].edges[node->edge];
    m_functions.Reset(m_shownEdge ? m_shownEdge->functions.size() : 0);
}

void CDependencyPanel::OnSelChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    ShowNode(NodeOf(reinterpret_cast<NMTREEVIEW*>(pNMHDR)->itemNew.hItem));
}

void CDependencyPanel::OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMTVCUSTOMDRAW* draw = reinterpret_cast<NMTVCUSTOMDRAW*>(pNMHDR);
    *pResult = CDRF_DODEFAULT;

    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *pResult = CDRF_NOTIFYITEMDRAW;
        return;
    }
    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT || (draw->nmcd.uItemState & CDIS_SELECTED))
        return;

    const Node* node = reinterpret_cast<const Node*>(draw->nmcd.lItemlParam);
    if (!node || !m_graph || node->edge == kNoEdge)
        return;
    const DepEdge& edge = m_graph->modules[node->importer].edges[node->edge];
    const DepModule& module = m_graph->modules[node->module];

    if (module.status == DepStatus::ApiSetNoHost)
        draw->clrText = edge.required ? kError : kMuted;
    else if (module.status != DepStatus::Ok || edge.missingFunctions)
        draw->clrText = edge.required ? kError : kWarning;
    else if (edge.kind != DepKind::Static)
        draw->clrText = kIndirect;
    else if (node->circular)
        draw->clrText = kMuted;
}

void CDependencyPanel::GoTo(size_t importer, size_t edge)
{
    if (!m_graph || importer >= m_graph->modules.size())
        return;

    // Shortest way down from the root, following the graph.
    std::vector<size_t> cameFromModule(m_graph->modules.size(), size_t(-1));
    std::vector<size_t> cameFromEdge(m_graph->modules.size(), size_t(-1));
    std::deque<size_t> queue;
    std::vector<bool> seen(m_graph->modules.size(), false);
    seen[0] = true;
    queue.push_back(0);
    while (!queue.empty() && !seen[importer])
    {
        const size_t id = queue.front();
        queue.pop_front();
        const auto& edges = m_graph->modules[id].edges;
        for (size_t e = 0; e < edges.size(); ++e)
            if (!seen[edges[e].target])
            {
                seen[edges[e].target] = true;
                cameFromModule[edges[e].target] = id;
                cameFromEdge[edges[e].target] = e;
                queue.push_back(edges[e].target);
            }
    }
    if (!seen[importer])
        return;

    std::vector<size_t> path;   // edge indexes, from the root down
    path.push_back(edge);
    for (size_t at = importer; at != 0; at = cameFromModule[at])
        path.insert(path.begin(), cameFromEdge[at]);

    HTREEITEM item = m_tree.GetRootItem();
    for (size_t step : path)
    {
        Node* node = NodeOf(item);
        if (!node)
            return;
        LoadChildren(*node);
        m_tree.Expand(item, TVE_EXPAND);
        // Children are in edge order.
        HTREEITEM child = m_tree.GetChildItem(item);
        for (size_t i = 0; child && i < step; ++i)
            child = m_tree.GetNextSiblingItem(child);
        if (!child)
            return;
        item = child;
    }
    m_tree.SelectItem(item);
    m_tree.EnsureVisible(item);
    m_tree.SetFocus();
}

void CDependencyPanel::OnRClick(NMHDR*, LRESULT* pResult)
{
    CPoint point;
    GetCursorPos(&point);
    CPoint client = point;
    m_tree.ScreenToClient(&client);
    UINT flags = 0;
    const HTREEITEM hit = m_tree.HitTest(client, &flags);
    if (hit)
    {
        m_tree.SelectItem(hit);
        ShowMenu(point);
    }
    *pResult = 1;
}

void CDependencyPanel::OnContextMenu(CWnd* pWnd, CPoint point)
{
    if (pWnd != &m_tree)
        return;
    if (point.x == -1 && point.y == -1)
    {
        // Keyboard: menu key or Shift+F10.
        CRect rc;
        const HTREEITEM sel = m_tree.GetSelectedItem();
        if (!sel || !m_tree.GetItemRect(sel, &rc, TRUE))
            return;
        point = rc.CenterPoint();
        m_tree.ClientToScreen(&point);
    }
    else
    {
        // Same as a right-click on the row, for a message that doesn't come through NM_RCLICK.
        CPoint client = point;
        m_tree.ScreenToClient(&client);
        UINT flags = 0;
        const HTREEITEM hit = m_tree.HitTest(client, &flags);
        if (!hit)
            return;
        m_tree.SelectItem(hit);
    }
    ShowMenu(point);
}

void CDependencyPanel::ShowMenu(CPoint point)
{
    const Node* node = NodeOf(m_tree.GetSelectedItem());
    if (!node || !m_graph)
        return;
    const DepModule& module = m_graph->modules[node->module];
    const UINT hasPath = module.path.empty() ? MF_GRAYED : 0;

    CMenu menu;
    menu.CreatePopupMenu();
    if (m_onOpen)
        menu.AppendMenu(MF_STRING | hasPath, cmdOpen, _T("&Analyse This Module"));
    menu.AppendMenu(MF_STRING | hasPath, cmdLocation, _T("Open File &Location"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, cmdCopyName, _T("Copy &Name"));
    menu.AppendMenu(MF_STRING | hasPath, cmdCopyPath, _T("&Copy Full Path"));

    switch (menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, this))
    {
    case cmdOpen:
        if (m_onOpen)
            m_onOpen(module.path);
        break;
    case cmdLocation:
        fmt::ShowInExplorer(GetSafeHwnd(), module.path, true);
        break;
    case cmdCopyName:
        fmt::CopyToClipboard(GetSafeHwnd(), fmt::Wide(module.name));
        break;
    case cmdCopyPath:
        fmt::CopyToClipboard(GetSafeHwnd(), fmt::Wide(module.path));
        break;
    }
}
