// Format.h : display helpers shared by the UI (from WinMon)
#pragma once

namespace fmt
{
    inline CString Bytes(double bytes)
    {
        static const TCHAR* units[] = { _T("B"), _T("KB"), _T("MB"), _T("GB"), _T("TB") };
        int u = 0;
        while (bytes >= 1024.0 && u < _countof(units) - 1)
        {
            bytes /= 1024.0;
            ++u;
        }
        CString s;
        if (u == 0)
            s.Format(_T("%.0f %s"), bytes, units[u]);
        else
            s.Format(bytes < 10.0 ? _T("%.2f %s") : bytes < 100.0 ? _T("%.1f %s") : _T("%.0f %s"), bytes, units[u]);
        return s;
    }

    inline CString Percent(double v)
    {
        CString s;
        s.Format(_T("%.1f %%"), v);
        return s;
    }

    // Thousands separators make long counts readable.
    inline CString Count(ULONGLONG v)
    {
        CString digits, out;
        digits.Format(_T("%I64u"), v);
        const int n = digits.GetLength();
        for (int i = 0; i < n; ++i)
        {
            out += digits[i];
            if ((n - i - 1) % 3 == 0 && i != n - 1)
                out += _T(',');
        }
        return out;
    }

    // Scales a 96-DPI pixel value to the current system DPI.
    inline int Dpi(int px)
    {
        static const int dpi = [] {
            HDC hdc = ::GetDC(nullptr);
            const int v = ::GetDeviceCaps(hdc, LOGPIXELSY);
            ::ReleaseDC(nullptr, hdc);
            return v;
        }();
        return ::MulDiv(px, dpi, 96);
    }

    inline CString Wide(const std::wstring& text)
    {
        return CString(text.c_str());
    }

    inline void CopyToClipboard(HWND owner, const CString& text)
    {
        if (!::OpenClipboard(owner))
            return;
        ::EmptyClipboard();
        const size_t bytes = (size_t(text.GetLength()) + 1) * sizeof(TCHAR);
        if (HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes))
        {
            memcpy(::GlobalLock(hMem), text.GetString(), bytes);
            ::GlobalUnlock(hMem);
            if (!::SetClipboardData(CF_UNICODETEXT, hMem))
                ::GlobalFree(hMem);
        }
        ::CloseClipboard();
    }

    // Opens Explorer with the file selected (or the folder itself).
    inline void ShowInExplorer(HWND owner, const std::wstring& path, bool selectFile)
    {
        CString args;
        args.Format(selectFile ? _T("/select,\"%s\"") : _T("\"%s\""), path.c_str());
        ::ShellExecute(owner, _T("open"), _T("explorer.exe"), args, nullptr, SW_SHOWNORMAL);
    }
}
