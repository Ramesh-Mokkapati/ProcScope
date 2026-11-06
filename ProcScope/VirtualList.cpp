// VirtualList.cpp
#include "pch.h"
#include "VirtualList.h"
#include "Format.h"

BEGIN_MESSAGE_MAP(CVirtualList, CMFCListCtrl)
    ON_NOTIFY_REFLECT(LVN_GETDISPINFO, &CVirtualList::OnGetDispInfo)
    ON_WM_CONTEXTMENU()
    ON_WM_KEYDOWN()
END_MESSAGE_MAP()

BOOL CVirtualList::CreateList(CWnd* pParent, UINT nID, bool multiSelect)
{
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS |
                        LVS_OWNERDATA | (multiSelect ? 0 : LVS_SINGLESEL);
    if (!Create(style, CRect(0, 0, 10, 10), pParent, nID))
        return FALSE;
    SetExtendedStyle(GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    return TRUE;
}

void CVirtualList::SetColumns(const std::vector<Column>& columns)
{
    m_columns = columns;
    for (int i = 0; i < int(columns.size()); ++i)
        InsertColumn(i, columns[i].title, columns[i].format, fmt::Dpi(columns[i].width));
}

void CVirtualList::SetSource(TextFn text, NumberFn number, ColorFn color)
{
    m_text = std::move(text);
    m_number = std::move(number);
    m_color = std::move(color);
}

CString CVirtualList::CellText(size_t modelRow, int column) const
{
    return m_text ? m_text(modelRow, column) : CString();
}

bool CVirtualList::Matches(size_t modelRow) const
{
    if (m_filter.IsEmpty())
        return true;
    for (int c = 0; c < int(m_columns.size()); ++c)
    {
        CString text = CellText(modelRow, c);
        text.MakeLower();
        if (text.Find(m_filter) >= 0)
            return true;
    }
    return false;
}

void CVirtualList::RebuildView()
{
    m_view.clear();
    m_view.reserve(m_filter.IsEmpty() ? m_modelCount : 0);
    for (size_t r = 0; r < m_modelCount; ++r)
        if (Matches(r))
            m_view.push_back(r);
    SortView();
}

void CVirtualList::SortView()
{
    const int col = m_iSortedColumn;
    if (col < 0 || col >= int(m_columns.size()) || m_view.size() < 2)
        return;
    const bool asc = m_bAscending != FALSE;

    // Keys are computed once, so sorting a whole-drive scan doesn't format strings per comparison.
    if (m_columns[col].numeric && m_number)
    {
        std::vector<double> keys(m_modelCount);
        for (size_t r : m_view)
            keys[r] = m_number(r, col);
        std::stable_sort(m_view.begin(), m_view.end(), [&](size_t a, size_t b) {
            return asc ? keys[a] < keys[b] : keys[a] > keys[b];
        });
    }
    else
    {
        std::vector<CString> keys(m_modelCount);
        for (size_t r : m_view)
            keys[r] = CellText(r, col);
        std::stable_sort(m_view.begin(), m_view.end(), [&](size_t a, size_t b) {
            const int cmp = StrCmpLogicalW(keys[a], keys[b]);
            return asc ? cmp < 0 : cmp > 0;
        });
    }
}

std::vector<size_t> CVirtualList::SelectedModelRows() const
{
    std::vector<size_t> rows;
    if (!GetSafeHwnd())
        return rows;
    for (int i = GetNextItem(-1, LVNI_SELECTED); i >= 0; i = GetNextItem(i, LVNI_SELECTED))
        if (i < int(m_view.size()))
            rows.push_back(m_view[i]);
    return rows;
}

void CVirtualList::PublishView(const std::vector<size_t>& selected, bool scrollToSelection)
{
    SetItemCountEx(int(m_view.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);

    if (!selected.empty())
    {
        std::unordered_set<size_t> wanted(selected.begin(), selected.end());
        int first = -1;
        for (int i = 0; i < int(m_view.size()) && !wanted.empty(); ++i)
        {
            if (wanted.erase(m_view[i]))
            {
                SetItemState(i, LVIS_SELECTED, LVIS_SELECTED);
                if (first < 0)
                    first = i;
            }
        }
        if (first >= 0)
        {
            SetItemState(first, LVIS_FOCUSED, LVIS_FOCUSED);
            if (scrollToSelection)
                EnsureVisible(first, FALSE);
        }
    }
    Invalidate(FALSE);
}

void CVirtualList::Reset(size_t count)
{
    m_modelCount = count;
    RebuildView();
    PublishView({}, false);
    if (!m_view.empty())
        EnsureVisible(0, FALSE);
}

void CVirtualList::Append(size_t count)
{
    // Rows only join at the end, so existing indices and the selection stay valid.
    const size_t before = m_modelCount;
    m_modelCount = count;
    for (size_t r = before; r < count; ++r)
        if (Matches(r))
            m_view.push_back(r);
    SetItemCountEx(int(m_view.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    Invalidate(FALSE);
}

void CVirtualList::Update(size_t count, const std::vector<size_t>& selected)
{
    m_modelCount = count;
    RebuildView();
    PublishView(selected, false);
}

void CVirtualList::SetFilter(const CString& filter)
{
    CString lowered(filter);
    lowered.MakeLower();
    if (lowered == m_filter)
        return;
    m_filter = lowered;
    const std::vector<size_t> selected = SelectedModelRows();
    RebuildView();
    PublishView(selected, true);
}

void CVirtualList::Resort()
{
    const std::vector<size_t> selected = SelectedModelRows();
    SortView();
    PublishView(selected, true);
}

void CVirtualList::Sort(int iColumn, BOOL bAscending, BOOL /*bAdd*/)
{
    // Sizes and counts are most useful largest-first, so a newly chosen numeric
    // column starts descending; clicking it again toggles the direction.
    if (iColumn != m_iSortedColumn && iColumn >= 0 && iColumn < int(m_columns.size()) && m_columns[iColumn].numeric)
        bAscending = FALSE;

    GetHeaderCtrl().SetSortColumn(iColumn, bAscending, FALSE);
    m_iSortedColumn = iColumn;
    m_bAscending = bAscending;
    Resort();
}

bool CVirtualList::GetSelectedRow(size_t& row) const
{
    const int sel = GetNextItem(-1, LVNI_SELECTED);
    if (sel < 0 || sel >= int(m_view.size()))
        return false;
    row = m_view[sel];
    return true;
}

void CVirtualList::SelectRow(size_t modelRow, bool scrollTo)
{
    PublishView({ modelRow }, scrollTo);
}

void CVirtualList::SelectAll()
{
    SetItemState(-1, LVIS_SELECTED, LVIS_SELECTED);
}

CString CVirtualList::RowText(size_t modelRow) const
{
    CString line;
    for (int c = 0; c < int(m_columns.size()); ++c)
    {
        if (c)
            line += _T('\t');
        line += CellText(modelRow, c);
    }
    return line;
}

CString CVirtualList::SelectedRowsText() const
{
    CString text;
    for (size_t r : SelectedModelRows())
    {
        text += RowText(r);
        text += _T("\r\n");
    }
    return text;
}

CString CVirtualList::AllRowsText() const
{
    CString text;
    for (int c = 0; c < int(m_columns.size()); ++c)
    {
        if (c)
            text += _T('\t');
        text += m_columns[c].title;
    }
    text += _T("\r\n");
    for (size_t r : m_view)
    {
        text += RowText(r);
        text += _T("\r\n");
    }
    return text;
}

void CVirtualList::OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    LVITEM& item = reinterpret_cast<NMLVDISPINFO*>(pNMHDR)->item;
    if (item.iItem < 0 || item.iItem >= int(m_view.size()))
        return;
    if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0)
        _tcsncpy_s(item.pszText, item.cchTextMax, CellText(m_view[item.iItem], item.iSubItem), _TRUNCATE);
}

COLORREF CVirtualList::OnGetCellTextColor(int nRow, int nColumn)
{
    if (m_color && nRow >= 0 && nRow < int(m_view.size()))
    {
        const COLORREF color = m_color(m_view[nRow]);
        if (color != CLR_DEFAULT)
            return color;
    }
    return CMFCListCtrl::OnGetCellTextColor(nRow, nColumn);
}

void CVirtualList::OnContextMenu(CWnd*, CPoint point)
{
    if (!m_onMenu)
        return;

    if (point.x == -1 && point.y == -1)
    {
        // Keyboard: menu key or Shift+F10.
        const int sel = GetNextItem(-1, LVNI_SELECTED);
        CRect rc;
        if (sel < 0 || !GetItemRect(sel, &rc, LVIR_LABEL))
            return;
        point = rc.CenterPoint();
        ClientToScreen(&point);
    }
    else
    {
        CPoint client = point;
        ScreenToClient(&client);

        // The header is a child window, but a right-click on it arrives here too.
        CRect header;
        GetHeaderCtrl().GetWindowRect(&header);
        if (header.PtInRect(point))
            return;

        UINT flags = 0;
        const int row = HitTest(client, &flags);
        if (row < 0)
            return;
        // Right-clicking outside the selection selects that row, as Explorer does.
        if (!(GetItemState(row, LVIS_SELECTED) & LVIS_SELECTED))
        {
            SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    m_onMenu(point);
}

void CVirtualList::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
    if (GetKeyState(VK_CONTROL) < 0)
    {
        if (nChar == 'C')
        {
            const CString text = SelectedRowsText();
            if (!text.IsEmpty())
                fmt::CopyToClipboard(GetSafeHwnd(), text);
            return;
        }
        if (nChar == 'A' && !(GetStyle() & LVS_SINGLESEL))
        {
            SelectAll();
            return;
        }
    }
    CMFCListCtrl::OnKeyDown(nChar, nRepCnt, nFlags);
}
