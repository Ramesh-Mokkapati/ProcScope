// VirtualList.h : owner-data, sortable, filterable report list.
// Rows stay in the owner's model and are read through callbacks, so the same control handles
// a handful of properties or a whole-drive scan with no per-row control overhead.
#pragma once

class CVirtualList : public CMFCListCtrl
{
public:
    struct Column
    {
        LPCTSTR title;
        int     width;
        bool    numeric = false;           // sorts on NumberFn, largest first on the first click
        int     format = LVCFMT_LEFT;
    };

    using TextFn   = std::function<CString(size_t row, int column)>;
    using NumberFn = std::function<double(size_t row, int column)>;
    using ColorFn  = std::function<COLORREF(size_t row)>;
    using MenuFn   = std::function<void(CPoint screenPoint)>;

    BOOL CreateList(CWnd* pParent, UINT nID, bool multiSelect);
    void SetColumns(const std::vector<Column>& columns);
    void SetSource(TextFn text, NumberFn number = nullptr, ColorFn color = nullptr);
    void SetContextMenuHandler(MenuFn handler) { m_onMenu = std::move(handler); }

    // The model now has `count` rows, all new: filters, sorts, and clears the selection.
    void Reset(size_t count);

    // The model grew to `count` rows. New rows go at the end, to avoid re-sorting on every tick of a scan.
    void Append(size_t count);

    // The whole model was replaced by newer data, as on a live refresh: filters and sorts again,
    // selects the given model rows, and keeps the scroll position.
    void Update(size_t count, const std::vector<size_t>& selected);

    // Case-insensitive match against all columns; empty shows everything.
    void SetFilter(const CString& filter);

    // Applies the current sort to rows added by Append.
    void Resort();

    size_t ModelCount() const { return m_modelCount; }
    size_t VisibleCount() const { return m_view.size(); }

    // Model row of the (first) selected row; false if nothing is selected.
    bool GetSelectedRow(size_t& row) const;
    void SelectRow(size_t modelRow, bool scrollTo = true);

    CString SelectedRowsText() const;   // tab-separated, one line per row
    CString AllRowsText() const;        // visible rows, with a header line
    CString CellText(size_t modelRow, int column) const;
    void    SelectAll();

    void Sort(int iColumn, BOOL bAscending = TRUE, BOOL bAdd = FALSE) override;

protected:
    COLORREF OnGetCellTextColor(int nRow, int nColumn) override;

    afx_msg void OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
    afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    DECLARE_MESSAGE_MAP()

private:
    bool Matches(size_t modelRow) const;
    void RebuildView();
    void SortView();
    void PublishView(const std::vector<size_t>& selected, bool scrollToSelection);
    std::vector<size_t> SelectedModelRows() const;
    CString RowText(size_t modelRow) const;

    std::vector<Column> m_columns;
    TextFn              m_text;
    NumberFn            m_number;
    ColorFn             m_color;
    MenuFn              m_onMenu;
    CString             m_filter;          // lower case
    size_t              m_modelCount = 0;
    std::vector<size_t> m_view;            // visible model rows, in display order
};
