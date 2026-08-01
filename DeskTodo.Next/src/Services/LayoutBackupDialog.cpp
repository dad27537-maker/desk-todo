#include "LayoutBackupDialog.h"

#include <shobjidl.h>

namespace {

std::wstring GetSelectedPath(IFileDialog* dialog) {
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) return L"";
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = path;
        CoTaskMemFree(path);
    }
    item->Release();
    return result;
}

void ConfigureDialog(IFileDialog* dialog) {
    const COMDLG_FILTERSPEC filters[] = {
        { L"DeskTodo 布局备份 (*.bak)", L"*.bak" },
        { L"所有文件 (*.*)", L"*.*" }
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"bak");
}

}

namespace LayoutBackupDialog {

std::wstring SelectSavePath(HWND owner) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))) || !dialog) return L"";
    ConfigureDialog(dialog);
    dialog->SetTitle(L"备份桌面布局");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t filename[96]{};
    swprintf_s(filename, L"DeskTodo-layout-%04u%02u%02u-%02u%02u%02u.bak",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    dialog->SetFileName(filename);
    const std::wstring result = SUCCEEDED(dialog->Show(owner))
        ? GetSelectedPath(dialog) : L"";
    dialog->Release();
    return result;
}

std::wstring SelectOpenPath(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))) || !dialog) return L"";
    ConfigureDialog(dialog);
    dialog->SetTitle(L"选择要还原的桌面布局备份");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    const std::wstring result = SUCCEEDED(dialog->Show(owner))
        ? GetSelectedPath(dialog) : L"";
    dialog->Release();
    return result;
}

}
