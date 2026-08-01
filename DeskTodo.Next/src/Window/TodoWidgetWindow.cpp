#include "TodoWidgetWindow.h"

#include "Services/DesktopPathService.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <windowsx.h>

namespace {

constexpr wchar_t WindowClass[] = L"DeskTodoTodoWidget";
constexpr int WindowWidth = 440;
constexpr int WindowHeight = 380;
constexpr int MinimumWindowWidth = 400;
constexpr int MinimumWindowHeight = 340;
constexpr int RowHeight = 52;
constexpr int AddButtonId = 5101;
constexpr int PriorityButtonId = 5102;
constexpr DWORD StateMagic = 0x4F444F54;
constexpr DWORD StateVersion = 2;
constexpr uint32_t MaximumPersistedTextLength = 4096;
constexpr uint32_t MaximumPersistedDateLength = 10;
constexpr COLORREF CardBackground = RGB(238, 234, 236);
constexpr COLORREF CardSurface = RGB(248, 245, 247);
constexpr COLORREF CardText = RGB(47, 43, 49);
constexpr COLORREF CardMutedText = RGB(112, 107, 114);

constexpr UINT TaskPriorityHighId = 5201;
constexpr UINT TaskPriorityNormalId = 5202;
constexpr UINT TaskPriorityLowId = 5203;
constexpr UINT TaskDueTodayId = 5211;
constexpr UINT TaskDueTomorrowId = 5212;
constexpr UINT TaskDueWeekId = 5213;
constexpr UINT TaskDueClearId = 5214;

template <typename T>
bool ReadValue(std::ifstream& stream, T& value) {
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
bool WriteValue(std::ofstream& stream, const T& value) {
    return static_cast<bool>(stream.write(
        reinterpret_cast<const char*>(&value), sizeof(value)));
}

bool ReadWideString(std::ifstream& stream, uint32_t length,
                    uint32_t maximumLength, std::wstring& value) {
    if (length > maximumLength) return false;
    value.assign(length, L'\0');
    return length == 0 || static_cast<bool>(stream.read(
        reinterpret_cast<char*>(value.data()),
        static_cast<std::streamsize>(length) * sizeof(wchar_t)));
}

bool WriteWideString(std::ofstream& stream, const std::wstring& value) {
    return value.empty() || static_cast<bool>(stream.write(
        reinterpret_cast<const char*>(value.data()),
        static_cast<std::streamsize>(value.size()) * sizeof(wchar_t)));
}

const wchar_t* PriorityLabel(TodoPriority priority) {
    switch (priority) {
    case TodoPriority::High: return L"高优先级";
    case TodoPriority::Low: return L"低优先级";
    case TodoPriority::Normal: return L"普通";
    }
    return L"普通";
}

COLORREF PriorityColor(TodoPriority priority) {
    switch (priority) {
    case TodoPriority::High: return RGB(224, 72, 72);
    case TodoPriority::Low: return RGB(83, 199, 157);
    case TodoPriority::Normal: return RGB(244, 166, 151);
    }
    return RGB(244, 166, 151);
}

const wchar_t* SortLabel(TodoSortMode mode) {
    switch (mode) {
    case TodoSortMode::Priority: return L"排序：优先级";
    case TodoSortMode::DueDate: return L"排序：截止日";
    case TodoSortMode::CompletedLast: return L"排序：完成项";
    }
    return L"排序：完成项";
}

std::wstring FormatIsoDate(const SYSTEMTIME& date) {
    wchar_t text[16]{};
    swprintf_s(text, L"%04u-%02u-%02u",
               date.wYear, date.wMonth, date.wDay);
    return text;
}

bool ParseIsoDate(const std::wstring& text, SYSTEMTIME& date) {
    if (!TodoList::IsValidDueDate(text) || text.empty()) return false;
    unsigned int year = 0;
    unsigned int month = 0;
    unsigned int day = 0;
    if (swscanf_s(text.c_str(), L"%4u-%2u-%2u",
                  &year, &month, &day) != 3) return false;
    date = {};
    date.wYear = static_cast<WORD>(year);
    date.wMonth = static_cast<WORD>(month);
    date.wDay = static_cast<WORD>(day);
    return true;
}

std::wstring DateAfterDays(int days) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&local, &fileTime)) return L"";
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    value.QuadPart += static_cast<ULONGLONG>(days) *
        24ULL * 60ULL * 60ULL * 10000000ULL;
    fileTime.dwLowDateTime = value.LowPart;
    fileTime.dwHighDateTime = value.HighPart;
    SYSTEMTIME result{};
    return FileTimeToSystemTime(&fileTime, &result)
        ? FormatIsoDate(result) : L"";
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void ApplyCardShape(HWND window) {
    RECT client{};
    if (!window || !GetClientRect(window, &client)) return;
    HRGN region = CreateRoundRectRgn(0, 0, client.right,
                                     client.bottom, 28, 28);
    if (region) SetWindowRgn(window, region, TRUE);
}

void DrawLabel(HDC dc, const std::wstring& text, RECT rect, HFONT font,
               COLORREF color, UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

}

TodoWidgetWindow::TodoWidgetWindow(HINSTANCE instance) : instance_(instance) {
    LoadState();
}

TodoWidgetWindow::~TodoWidgetWindow() { Destroy(); }

void TodoWidgetWindow::RestoreIfVisible() {
    if (shouldRestore_) ShowOrCreate();
}

void TodoWidgetWindow::ShowOrCreate() {
    if (!EnsureWindow()) return;
    ShowWindow(window_, SW_SHOWNORMAL);
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    SetFocus(input_);
    shouldRestore_ = true;
    SaveState(true);
}

void TodoWidgetWindow::Destroy() {
    if (!window_) return;
    if (editingIndex_ >= 0) EndEditTask(true);
    SaveState(IsWindowVisible(window_) == TRUE);
    DestroyWindow(window_);
    window_ = nullptr;
}

bool TodoWidgetWindow::EnsureWindow() {
    if (window_ && IsWindow(window_)) return true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{ sizeof(cls) };
        cls.lpfnWndProc = StaticWindowProc;
        cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = nullptr;
        cls.lpszClassName = WindowClass;
        registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return false;

    RECT position = savedRect_;
    if (position.right <= position.left || position.bottom <= position.top ||
        !MonitorFromRect(&position, MONITOR_DEFAULTTONULL)) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        position.left = monitor.rcWork.right - WindowWidth - 28;
        position.top = monitor.rcWork.bottom - WindowHeight - 250;
    }
    const int savedWidth = position.right - position.left;
    const int savedHeight = position.bottom - position.top;
    const int windowWidth = (std::max)(WindowWidth, savedWidth);
    const int windowHeight = (std::max)(WindowHeight, savedHeight);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        WindowClass, L"Todo List",
        WS_POPUP | WS_THICKFRAME,
        position.left, position.top, windowWidth, windowHeight,
        nullptr, nullptr, instance_, this);
    if (!window_) return false;
    SetLayeredWindowAttributes(window_, 0, 226, LWA_ALPHA);
    ApplyCardShape(window_);
    return true;
}

void TodoWidgetWindow::LoadState() {
    const std::filesystem::path path =
        std::filesystem::path(GetAppDataPath()) / L"todo-widget.dat";
    std::ifstream stream(path, std::ios::binary);
    DWORD magic = 0;
    DWORD version = 0;
    if (!ReadValue(stream, magic) || !ReadValue(stream, version) ||
        magic != StateMagic) return;

    RECT loadedRect{};
    bool loadedVisible = false;
    TodoSortMode loadedSort = TodoSortMode::CompletedLast;
    uint32_t taskCount = 0;
    bool migrateLegacyState = false;

    if (version == 1) {
        BOOL visible = FALSE;
        if (!ReadValue(stream, visible) ||
            !ReadValue(stream, loadedRect) ||
            !ReadValue(stream, taskCount) ||
            taskCount > TodoList::MaxItems) return;
        loadedVisible = visible == TRUE;
        migrateLegacyState = true;
    } else if (version == StateVersion) {
        uint8_t visible = 0;
        uint8_t sort = 0;
        if (!ReadValue(stream, visible) ||
            !ReadValue(stream, loadedRect.left) ||
            !ReadValue(stream, loadedRect.top) ||
            !ReadValue(stream, loadedRect.right) ||
            !ReadValue(stream, loadedRect.bottom) ||
            !ReadValue(stream, taskCount) ||
            !ReadValue(stream, sort) ||
            taskCount > TodoList::MaxItems ||
            sort > static_cast<uint8_t>(TodoSortMode::DueDate)) return;
        loadedVisible = visible != 0;
        loadedSort = static_cast<TodoSortMode>(sort);
    } else {
        return;
    }

    std::vector<TodoItem> loadedItems;
    loadedItems.reserve(taskCount);
    std::unordered_set<uint64_t> usedIds;
    for (uint32_t index = 0; index < taskCount; ++index) {
        TodoItem item;
        uint8_t completed = 0;
        if (version == 1) {
            uint32_t textLength = 0;
            if (!ReadValue(stream, completed) ||
                !ReadValue(stream, textLength) ||
                !ReadWideString(stream, textLength,
                                MaximumPersistedTextLength, item.text)) return;
            item.id = static_cast<uint64_t>(index) + 1;
        } else {
            uint8_t priority = 0;
            uint32_t dueDateLength = 0;
            uint32_t textLength = 0;
            if (!ReadValue(stream, item.id) ||
                !ReadValue(stream, completed) ||
                !ReadValue(stream, priority) ||
                !ReadValue(stream, dueDateLength) ||
                !ReadValue(stream, textLength) ||
                priority > static_cast<uint8_t>(TodoPriority::High) ||
                !ReadWideString(stream, dueDateLength,
                                MaximumPersistedDateLength, item.dueDate) ||
                !ReadWideString(stream, textLength,
                                MaximumPersistedTextLength, item.text) ||
                !TodoList::IsValidDueDate(item.dueDate)) return;
            item.priority = static_cast<TodoPriority>(priority);
            if (item.id == 0 || usedIds.find(item.id) != usedIds.end()) {
                item.id = TodoList::NextId(loadedItems);
            }
        }
        item.completed = completed != 0;
        usedIds.insert(item.id);
        loadedItems.push_back(std::move(item));
    }

    stream.close();
    savedRect_ = loadedRect;
    shouldRestore_ = loadedVisible;
    sortMode_ = loadedSort;
    items_ = std::move(loadedItems);
    ApplySort();
    if (migrateLegacyState) SaveState(shouldRestore_);
}

void TodoWidgetWindow::SaveState(bool visible) {
    EnsureDirectoryExists(GetAppDataPath());
    const std::filesystem::path path =
        std::filesystem::path(GetAppDataPath()) / L"todo-widget.dat";
    const std::filesystem::path temporaryPath =
        std::filesystem::path(GetAppDataPath()) / L"todo-widget.dat.tmp";

    RECT rect = savedRect_;
    if (window_ && IsWindow(window_)) GetWindowRect(window_, &rect);
    savedRect_ = rect;

    std::ofstream stream(temporaryPath,
                         std::ios::binary | std::ios::trunc);
    const uint8_t savedVisible = visible ? 1 : 0;
    const uint32_t taskCount = static_cast<uint32_t>(items_.size());
    const uint8_t sort = static_cast<uint8_t>(sortMode_);
    bool ok = WriteValue(stream, StateMagic) &&
        WriteValue(stream, StateVersion) &&
        WriteValue(stream, savedVisible) &&
        WriteValue(stream, rect.left) &&
        WriteValue(stream, rect.top) &&
        WriteValue(stream, rect.right) &&
        WriteValue(stream, rect.bottom) &&
        WriteValue(stream, taskCount) &&
        WriteValue(stream, sort);
    for (const auto& item : items_) {
        const uint8_t completed = item.completed ? 1 : 0;
        const uint8_t priority = static_cast<uint8_t>(item.priority);
        const uint32_t dueDateLength =
            static_cast<uint32_t>(item.dueDate.size());
        const uint32_t textLength =
            static_cast<uint32_t>(item.text.size());
        ok = ok &&
            WriteValue(stream, item.id) &&
            WriteValue(stream, completed) &&
            WriteValue(stream, priority) &&
            WriteValue(stream, dueDateLength) &&
            WriteValue(stream, textLength) &&
            WriteWideString(stream, item.dueDate) &&
            WriteWideString(stream, item.text);
        if (!ok) break;
    }
    stream.flush();
    ok = ok && stream.good();
    stream.close();
    if (ok) {
        ok = MoveFileExW(temporaryPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH) == TRUE;
    }
    if (!ok) DeleteFileW(temporaryPath.c_str());
}

void TodoWidgetWindow::AddInputTask() {
    if (editingIndex_ >= 0) EndEditTask(true);
    const int length = GetWindowTextLengthW(input_);
    std::wstring text(static_cast<size_t>((std::max)(0, length)) + 1, L'\0');
    if (length > 0) GetWindowTextW(input_, text.data(), length + 1);
    text.resize(static_cast<size_t>((std::max)(0, length)));
    if (!TodoList::Add(items_, text, inputPriority_, ReadDueDate())) return;
    const uint64_t addedId = items_.back().id;
    ApplySort();
    SetWindowTextW(input_, L"");
    ResetInputMetadata();
    const auto added = std::find_if(items_.begin(), items_.end(),
        [addedId](const TodoItem& item) { return item.id == addedId; });
    if (added != items_.end()) {
        const int index = static_cast<int>(std::distance(items_.begin(), added));
        if (index < firstVisible_) firstVisible_ = index;
        if (index >= firstVisible_ + VisibleRowCount()) {
            firstVisible_ = index - VisibleRowCount() + 1;
        }
    }
    ClampScroll();
    SaveState(true);
    InvalidateRect(window_, nullptr, FALSE);
}

float TodoWidgetWindow::UiScale() const {
    RECT client{};
    if (!window_ || !GetClientRect(window_, &client) ||
        client.right <= 0 || client.bottom <= 0) {
        return 1.0f;
    }
    const float widthScale = static_cast<float>(client.right) /
        static_cast<float>(WindowWidth);
    const float heightScale = static_cast<float>(client.bottom) /
        static_cast<float>(WindowHeight);
    return (std::clamp)((std::max)(widthScale, heightScale), 0.92f, 1.35f);
}

int TodoWidgetWindow::Scale(int value) const {
    return (std::max)(1, static_cast<int>(std::lround(
        static_cast<float>(value) * UiScale())));
}

void TodoWidgetWindow::ClampScroll() {
    firstVisible_ = (std::clamp)(firstVisible_, 0,
        (std::max)(0, static_cast<int>(items_.size()) - VisibleRowCount()));
}

int TodoWidgetWindow::VisibleRowCount() const {
    RECT client{};
    if (!window_ || !GetClientRect(window_, &client)) return 4;
    return (std::max)(1, (static_cast<int>(client.bottom) - Scale(164)) /
        Scale(RowHeight));
}

RECT TodoWidgetWindow::AddButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { client.right - Scale(68), client.bottom - Scale(46),
             client.right - Scale(20), client.bottom - Scale(14) };
}

RECT TodoWidgetWindow::CloseButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { client.right - Scale(38), Scale(10),
             client.right - Scale(12), Scale(36) };
}

RECT TodoWidgetWindow::SortButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { (std::max)(Scale(190), static_cast<int>(client.right) - Scale(178)), Scale(10),
             client.right - Scale(46), Scale(36) };
}

RECT TodoWidgetWindow::PriorityButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { Scale(20), client.bottom - Scale(82), Scale(112),
             client.bottom - Scale(52) };
}

RECT TodoWidgetWindow::DueDatePickerRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { Scale(122), client.bottom - Scale(82),
             client.right - Scale(20), client.bottom - Scale(52) };
}

RECT TodoWidgetWindow::RowRect(int row) const {
    RECT client{};
    GetClientRect(window_, &client);
    const int top = Scale(72) + row * Scale(RowHeight);
    return { Scale(20), top, client.right - Scale(20),
             top + Scale(RowHeight - 4) };
}

void TodoWidgetWindow::LayoutControls() {
    if (!window_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const RECT add = AddButtonRect();
    if (input_) MoveWindow(input_, Scale(20), client.bottom - Scale(46),
                           (std::max)(Scale(80), static_cast<int>(add.left) - Scale(30)),
                           Scale(32), TRUE);
    if (addButton_) MoveWindow(addButton_, add.left, add.top,
                              add.right - add.left, add.bottom - add.top, TRUE);
    const RECT priority = PriorityButtonRect();
    if (priorityButton_) MoveWindow(
        priorityButton_, priority.left, priority.top,
        priority.right - priority.left, priority.bottom - priority.top, TRUE);
    const RECT dueDate = DueDatePickerRect();
    if (dueDatePicker_) MoveWindow(
        dueDatePicker_, dueDate.left, dueDate.top,
        dueDate.right - dueDate.left, dueDate.bottom - dueDate.top, TRUE);
    if (editingIndex_ >= firstVisible_ &&
        editingIndex_ < firstVisible_ + VisibleRowCount() && itemEditor_) {
        const RECT row = RowRect(editingIndex_ - firstVisible_);
        MoveWindow(itemEditor_, row.left + Scale(38), row.top + Scale(3),
                   row.right - row.left - Scale(70), Scale(25), TRUE);
    }
}

void TodoWidgetWindow::UpdateControlFont() {
    HFONT nextFont = CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
        L"Segoe UI Variable Text");
    if (!nextFont) return;
    HFONT oldFont = inputFont_;
    inputFont_ = nextFont;
    if (input_) SendMessageW(input_, WM_SETFONT,
                             reinterpret_cast<WPARAM>(inputFont_), TRUE);
    if (itemEditor_) SendMessageW(itemEditor_, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(inputFont_), TRUE);
    if (priorityButton_) SendMessageW(priorityButton_, WM_SETFONT,
                                      reinterpret_cast<WPARAM>(inputFont_), TRUE);
    if (dueDatePicker_) SendMessageW(dueDatePicker_, WM_SETFONT,
                                     reinterpret_cast<WPARAM>(inputFont_), TRUE);
    if (oldFont) DeleteObject(oldFont);
}

void TodoWidgetWindow::ApplySort() {
    TodoList::Sort(items_, sortMode_);
    ClampScroll();
}

void TodoWidgetWindow::CycleSortMode() {
    if (editingIndex_ >= 0) EndEditTask(true);
    const uint8_t next =
        (static_cast<uint8_t>(sortMode_) + 1) % 3;
    sortMode_ = static_cast<TodoSortMode>(next);
    ApplySort();
    firstVisible_ = 0;
    SaveState(true);
    InvalidateRect(window_, nullptr, FALSE);
}

void TodoWidgetWindow::CycleInputPriority() {
    switch (inputPriority_) {
    case TodoPriority::Normal:
        inputPriority_ = TodoPriority::High;
        break;
    case TodoPriority::High:
        inputPriority_ = TodoPriority::Low;
        break;
    case TodoPriority::Low:
        inputPriority_ = TodoPriority::Normal;
        break;
    }
    if (priorityButton_) InvalidateRect(priorityButton_, nullptr, TRUE);
}

void TodoWidgetWindow::ResetInputMetadata() {
    inputPriority_ = TodoPriority::Normal;
    if (priorityButton_) InvalidateRect(priorityButton_, nullptr, TRUE);
    if (dueDatePicker_) {
        SendMessageW(dueDatePicker_, DTM_SETSYSTEMTIME, GDT_NONE, 0);
    }
}

std::wstring TodoWidgetWindow::ReadDueDate() const {
    if (!dueDatePicker_) return L"";
    SYSTEMTIME date{};
    return SendMessageW(dueDatePicker_, DTM_GETSYSTEMTIME, 0,
                        reinterpret_cast<LPARAM>(&date)) == GDT_VALID
        ? FormatIsoDate(date) : L"";
}

void TodoWidgetWindow::SetDueDate(const std::wstring& dueDate) {
    if (!dueDatePicker_) return;
    SYSTEMTIME date{};
    if (ParseIsoDate(dueDate, date)) {
        SendMessageW(dueDatePicker_, DTM_SETSYSTEMTIME, GDT_VALID,
                     reinterpret_cast<LPARAM>(&date));
    } else {
        SendMessageW(dueDatePicker_, DTM_SETSYSTEMTIME, GDT_NONE, 0);
    }
}

void TodoWidgetWindow::ShowTaskContextMenu(int index, POINT screenPoint) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const uint64_t targetId = items_[index].id;
    if (editingIndex_ >= 0) EndEditTask(true);
    const auto target = std::find_if(items_.begin(), items_.end(),
        [targetId](const TodoItem& item) { return item.id == targetId; });
    if (target == items_.end()) return;
    const TodoItem snapshot = *target;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING |
        (snapshot.priority == TodoPriority::High ? MF_CHECKED : 0),
        TaskPriorityHighId, L"高优先级");
    AppendMenuW(menu, MF_STRING |
        (snapshot.priority == TodoPriority::Normal ? MF_CHECKED : 0),
        TaskPriorityNormalId, L"普通优先级");
    AppendMenuW(menu, MF_STRING |
        (snapshot.priority == TodoPriority::Low ? MF_CHECKED : 0),
        TaskPriorityLowId, L"低优先级");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TaskDueTodayId, L"截止：今天");
    AppendMenuW(menu, MF_STRING, TaskDueTomorrowId, L"截止：明天");
    AppendMenuW(menu, MF_STRING, TaskDueWeekId, L"截止：一周后");
    AppendMenuW(menu, MF_STRING |
        (snapshot.dueDate.empty() ? MF_GRAYED : 0),
        TaskDueClearId, L"清除截止日期");

    SetForegroundWindow(window_);
    const UINT selected = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (selected == 0) return;

    auto item = std::find_if(items_.begin(), items_.end(),
        [&](const TodoItem& candidate) {
            return candidate.id == snapshot.id;
        });
    if (item == items_.end()) return;
    switch (selected) {
    case TaskPriorityHighId:
        item->priority = TodoPriority::High;
        break;
    case TaskPriorityNormalId:
        item->priority = TodoPriority::Normal;
        break;
    case TaskPriorityLowId:
        item->priority = TodoPriority::Low;
        break;
    case TaskDueTodayId:
        item->dueDate = DateAfterDays(0);
        break;
    case TaskDueTomorrowId:
        item->dueDate = DateAfterDays(1);
        break;
    case TaskDueWeekId:
        item->dueDate = DateAfterDays(7);
        break;
    case TaskDueClearId:
        item->dueDate.clear();
        break;
    default:
        return;
    }
    ApplySort();
    SaveState(true);
    InvalidateRect(window_, nullptr, FALSE);
}

void TodoWidgetWindow::BeginEditTask(int index, int visibleRow) {
    if (index < 0 || index >= static_cast<int>(items_.size()) || !itemEditor_) return;
    if (editingIndex_ >= 0) EndEditTask(true);
    editingIndex_ = index;
    inputPriority_ = items_[index].priority;
    SetDueDate(items_[index].dueDate);
    if (priorityButton_) InvalidateRect(priorityButton_, nullptr, TRUE);
    const RECT row = RowRect(visibleRow);
    SetWindowTextW(itemEditor_, items_[index].text.c_str());
    MoveWindow(itemEditor_, row.left + 38, row.top + 3,
               row.right - row.left - 70, 25, TRUE);
    ShowWindow(itemEditor_, SW_SHOW);
    SetFocus(itemEditor_);
    SendMessageW(itemEditor_, EM_SETSEL, 0, -1);
    InvalidateRect(window_, &row, FALSE);
}

void TodoWidgetWindow::EndEditTask(bool commit) {
    if (editingIndex_ < 0 || !itemEditor_) return;
    const int index = editingIndex_;
    const uint64_t editedId =
        index < static_cast<int>(items_.size()) ? items_[index].id : 0;
    editingIndex_ = -1;
    bool changed = false;
    if (commit && index < static_cast<int>(items_.size())) {
        const int length = GetWindowTextLengthW(itemEditor_);
        std::wstring text(static_cast<size_t>((std::max)(0, length)) + 1, L'\0');
        if (length > 0) GetWindowTextW(itemEditor_, text.data(), length + 1);
        text.resize(static_cast<size_t>((std::max)(0, length)));
        if (TodoList::Update(items_, static_cast<size_t>(index), text)) {
            items_[index].priority = inputPriority_;
            items_[index].dueDate = ReadDueDate();
            changed = true;
        }
    }
    ShowWindow(itemEditor_, SW_HIDE);
    ResetInputMetadata();
    if (changed) {
        ApplySort();
        const auto edited = std::find_if(items_.begin(), items_.end(),
            [editedId](const TodoItem& item) { return item.id == editedId; });
        if (edited != items_.end()) {
            const int sortedIndex =
                static_cast<int>(std::distance(items_.begin(), edited));
            if (sortedIndex < firstVisible_) firstVisible_ = sortedIndex;
            if (sortedIndex >= firstVisible_ + VisibleRowCount()) {
                firstVisible_ = sortedIndex - VisibleRowCount() + 1;
            }
            ClampScroll();
        }
        SaveState(true);
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void TodoWidgetWindow::Paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    FillRounded(dc, client, 28, CardBackground);
    HFONT titleFont = CreateFontW(-Scale(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Display");
    HFONT bodyFont = CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Text");
    HFONT doneFont = CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Text");
    HFONT smallFont = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Text");

    const int remaining = TodoList::Remaining(items_);
    const std::wstring title = L"今日清单 · 剩余 " + std::to_wstring(remaining);
    const RECT sortButton = SortButtonRect();
    DrawLabel(dc, title, { Scale(20), Scale(13), sortButton.left - Scale(8), Scale(39) },
              titleFont, CardText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    FillRounded(dc, sortButton, Scale(12), RGB(220, 215, 220));
    DrawLabel(dc, SortLabel(sortMode_), sortButton, smallFont,
              CardMutedText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawLabel(dc, L"×", CloseButtonRect(), titleFont, CardMutedText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT track{ Scale(20), Scale(50), client.right - Scale(20), Scale(55) };
    FillRounded(dc, track, Scale(5), RGB(216, 211, 216));
    if (!items_.empty()) {
        const double completed = static_cast<double>(items_.size() - remaining) / items_.size();
        RECT progress = track;
        progress.right = progress.left + static_cast<int>((track.right - track.left) * completed);
        if (progress.right > progress.left) {
            FillRounded(dc, progress, Scale(5), RGB(151, 140, 153));
        }
    }

    const std::wstring today = DateAfterDays(0);
    for (int row = 0; row < VisibleRowCount(); ++row) {
        const int index = firstVisible_ + row;
        if (index >= static_cast<int>(items_.size())) break;
        const TodoItem& item = items_[index];
        const RECT rowRect = RowRect(row);
        FillRounded(dc, rowRect, Scale(10), CardSurface);
        RECT priorityStrip{
            rowRect.left + Scale(5), rowRect.top + Scale(7),
            rowRect.left + Scale(9), rowRect.bottom - Scale(7)
        };
        FillRounded(dc, priorityStrip, Scale(4), PriorityColor(item.priority));
        RECT checkbox{
            rowRect.left + Scale(14), rowRect.top + Scale(15),
            rowRect.left + Scale(32), rowRect.top + Scale(33)
        };
        FillRounded(dc, checkbox, Scale(5),
                    item.completed ? RGB(151, 140, 153) : RGB(76, 72, 80));
        if (item.completed) {
            DrawLabel(dc, L"✓", checkbox, bodyFont, RGB(255, 255, 255),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT textRect{
            rowRect.left + Scale(40),
            rowRect.top + Scale(item.dueDate.empty() ? 5 : 2),
            rowRect.right - Scale(54),
            rowRect.top + Scale(item.dueDate.empty() ? 42 : 25)
        };
        if (index != editingIndex_) {
            DrawLabel(dc, item.text, textRect, item.completed ? doneFont : bodyFont,
                      item.completed ? RGB(145, 138, 146) : CardText,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (!item.dueDate.empty()) {
            const bool overdue = !item.completed &&
                item.dueDate < today;
            RECT dateRect{
                rowRect.left + Scale(40), rowRect.top + Scale(25),
                rowRect.right - Scale(54), rowRect.bottom - Scale(3)
            };
            DrawLabel(dc, L"截止 " + item.dueDate, dateRect, smallFont,
                      overdue ? RGB(208, 65, 65) :
                      (item.completed ? RGB(156, 149, 158) :
                       CardMutedText),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                      DT_END_ELLIPSIS);
        }
        RECT editHint{ rowRect.right - Scale(52), rowRect.top,
                       rowRect.right - Scale(28), rowRect.bottom };
        DrawLabel(dc, L"✎", editHint, bodyFont, CardMutedText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT deleteRect{ rowRect.right - Scale(28), rowRect.top,
                         rowRect.right - Scale(4), rowRect.bottom };
        DrawLabel(dc, L"×", deleteRect, bodyFont, RGB(135, 130, 140),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (items_.empty()) {
        RECT emptyRect{ Scale(30), Scale(92), client.right - Scale(30),
                        client.bottom - Scale(102) };
        DrawLabel(dc, L"还没有待办事项", emptyRect, bodyFont,
                  CardMutedText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(doneFont);
    DeleteObject(smallFont);
}

LRESULT CALLBACK TodoWidgetWindow::StaticWindowProc(HWND window, UINT message,
                                                     WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TodoWidgetWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<TodoWidgetWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->WindowProc(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

LRESULT CALLBACK TodoWidgetWindow::EditSubclassProc(HWND edit, UINT message,
                                                     WPARAM wp, LPARAM lp,
                                                     UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<TodoWidgetWindow*>(data);
    if (message == WM_KEYDOWN && wp == VK_RETURN && self) {
        if (edit == self->input_) self->AddInputTask();
        else self->EndEditTask(true);
        return 0;
    }
    if (message == WM_KEYDOWN && wp == VK_ESCAPE && self && edit == self->itemEditor_) {
        self->EndEditTask(false);
        return 0;
    }
    if (message == WM_SETFOCUS && self && edit == self->input_ &&
        self->editingIndex_ >= 0) {
        self->EndEditTask(true);
    }
    if (message == WM_KILLFOCUS && self && edit == self->itemEditor_ &&
        self->editingIndex_ >= 0) {
        const HWND next = reinterpret_cast<HWND>(wp);
        const bool metadataControl =
            next == self->priorityButton_ ||
            next == self->dueDatePicker_ ||
            (self->dueDatePicker_ && IsChild(self->dueDatePicker_, next));
        if (!metadataControl) self->EndEditTask(true);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, EditSubclassProc, id);
    return DefSubclassProc(edit, message, wp, lp);
}

LRESULT TodoWidgetWindow::WindowProc(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX commonControls{
            sizeof(commonControls), ICC_DATE_CLASSES
        };
        InitCommonControlsEx(&commonControls);
        inputBrush_ = CreateSolidBrush(CardSurface);
        UpdateControlFont();
        input_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_AUTOHSCROLL, 20, 258, 262, 32, window_, nullptr, instance_, nullptr);
        addButton_ = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | BS_OWNERDRAW, 292, 258, 48, 32, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(AddButtonId)), instance_, nullptr);
        itemEditor_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        priorityButton_ = CreateWindowExW(
            0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(PriorityButtonId)),
            instance_, nullptr);
        dueDatePicker_ = CreateWindowExW(
            0, DATETIMEPICK_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            DTS_SHORTDATEFORMAT | DTS_SHOWNONE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        UpdateControlFont();
        SendMessageW(input_, EM_SETLIMITTEXT,
                     TodoList::MaxTextLength, 0);
        SendMessageW(itemEditor_, EM_SETLIMITTEXT,
                     TodoList::MaxTextLength, 0);
        SendMessageW(input_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"添加一项，回车保存"));
        SendMessageW(dueDatePicker_, DTM_SETFORMATW, 0,
                     reinterpret_cast<LPARAM>(
                         const_cast<wchar_t*>(L"yyyy-MM-dd")));
        SetWindowSubclass(input_, EditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(itemEditor_, EditSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
        ResetInputMetadata();
        LayoutControls();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = { MinimumWindowWidth, MinimumWindowHeight };
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wp) {
            auto* size = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            size->rgrc[0] = size->rgrc[1];
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(window_, &point);
        RECT client{};
        GetClientRect(window_, &client);
        constexpr int edge = 8;
        const bool left = point.x < client.left + edge;
        const bool right = point.x >= client.right - edge;
        const bool top = point.y < client.top + edge;
        const bool bottom = point.y >= client.bottom - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_SIZE:
        ApplyCardShape(window_);
        UpdateControlFont();
        LayoutControls();
        ClampScroll();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == AddButtonId) {
            AddInputTask();
            return 0;
        }
        if (LOWORD(wp) == PriorityButtonId &&
            HIWORD(wp) == BN_CLICKED) {
            CycleInputPriority();
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw && draw->CtlID == AddButtonId) {
            FillRounded(draw->hDC, draw->rcItem, Scale(10),
                        draw->itemState & ODS_SELECTED ? RGB(76, 70, 80) : RGB(91, 84, 94));
            HFONT font = CreateFontW(-Scale(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Segoe UI Variable Display");
            DrawLabel(draw->hDC, L"+", draw->rcItem, font, RGB(255, 255, 255),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(font);
            return TRUE;
        }
        if (draw && draw->CtlID == PriorityButtonId) {
            const COLORREF color = PriorityColor(inputPriority_);
            FillRounded(draw->hDC, draw->rcItem, Scale(8), color);
            HFONT font = CreateFontW(-Scale(13), 0, 0, 0, FW_SEMIBOLD,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH,
                L"Segoe UI Variable Text");
            DrawLabel(draw->hDC, PriorityLabel(inputPriority_),
                      draw->rcItem, font,
                      inputPriority_ == TodoPriority::Low
                          ? RGB(20, 78, 70) : RGB(255, 255, 255),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(font);
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        HDC memory = CreateCompatibleDC(dc);
        RECT client{};
        GetClientRect(window_, &client);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        Paint(memory);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), CardText);
        SetBkColor(reinterpret_cast<HDC>(wp), CardSurface);
        return reinterpret_cast<LRESULT>(inputBrush_);
    case WM_LBUTTONDOWN: {
        POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const RECT close = CloseButtonRect();
        if (PtInRect(&close, point)) {
            if (editingIndex_ >= 0) EndEditTask(true);
            ShowWindow(window_, SW_HIDE);
            shouldRestore_ = false;
            SaveState(false);
            return 0;
        }
        const RECT sort = SortButtonRect();
        if (PtInRect(&sort, point)) {
            CycleSortMode();
            return 0;
        }
        for (int row = 0; row < VisibleRowCount(); ++row) {
            const int visibleIndex = firstVisible_ + row;
            if (visibleIndex >= static_cast<int>(items_.size())) break;
            const RECT rowRect = RowRect(row);
            if (!PtInRect(&rowRect, point)) continue;
            const uint64_t targetId = items_[visibleIndex].id;
            if (editingIndex_ >= 0) EndEditTask(true);
            const auto target = std::find_if(items_.begin(), items_.end(),
                [targetId](const TodoItem& item) {
                    return item.id == targetId;
                });
            if (target == items_.end()) return 0;
            const int index =
                static_cast<int>(std::distance(items_.begin(), target));
            if (point.x >= rowRect.right - 28) {
                TodoList::Remove(items_, index);
                ApplySort();
            } else if (point.x <= rowRect.left + 11) {
                switch (items_[index].priority) {
                case TodoPriority::Normal:
                    items_[index].priority = TodoPriority::High;
                    break;
                case TodoPriority::High:
                    items_[index].priority = TodoPriority::Low;
                    break;
                case TodoPriority::Low:
                    items_[index].priority = TodoPriority::Normal;
                    break;
                }
                ApplySort();
            } else if (point.x <= rowRect.left + 36) {
                TodoList::Toggle(items_, index);
                ApplySort();
            } else {
                if (index < firstVisible_) firstVisible_ = index;
                if (index >= firstVisible_ + VisibleRowCount()) {
                    firstVisible_ = index - VisibleRowCount() + 1;
                }
                ClampScroll();
                BeginEditTask(index, index - firstVisible_);
                return 0;
            }
            ClampScroll();
            SaveState(true);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (point.y < 48) {
            dragging_ = true;
            GetPhysicalCursorPos(&dragStart_);
            GetWindowRect(window_, &dragWindowStart_);
            SetCapture(window_);
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT screenPoint{
            static_cast<short>(LOWORD(lp)),
            static_cast<short>(HIWORD(lp))
        };
        if (screenPoint.x == -1 && screenPoint.y == -1) {
            GetPhysicalCursorPos(&screenPoint);
        }
        POINT clientPoint = screenPoint;
        ScreenToClient(window_, &clientPoint);
        for (int row = 0; row < VisibleRowCount(); ++row) {
            const int index = firstVisible_ + row;
            if (index >= static_cast<int>(items_.size())) break;
            const RECT rowRect = RowRect(row);
            if (PtInRect(&rowRect, clientPoint)) {
                ShowTaskContextMenu(index, screenPoint);
                return 0;
            }
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && editingIndex_ >= 0) {
            EndEditTask(true);
        }
        break;
    case WM_MOUSEMOVE:
        if (dragging_) {
            POINT cursor{};
            GetPhysicalCursorPos(&cursor);
            SetWindowPos(window_, nullptr,
                dragWindowStart_.left + cursor.x - dragStart_.x,
                dragWindowStart_.top + cursor.y - dragStart_.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            SaveState(true);
        }
        return 0;
    case WM_EXITSIZEMOVE:
        SaveState(true);
        return 0;
    case WM_MOUSEWHEEL:
        if (editingIndex_ >= 0) EndEditTask(true);
        firstVisible_ += GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : -1;
        ClampScroll();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_CLOSE:
        if (editingIndex_ >= 0) EndEditTask(true);
        ShowWindow(window_, SW_HIDE);
        shouldRestore_ = false;
        SaveState(false);
        return 0;
    case WM_DESTROY:
        if (inputFont_) { DeleteObject(inputFont_); inputFont_ = nullptr; }
        if (inputBrush_) { DeleteObject(inputBrush_); inputBrush_ = nullptr; }
        input_ = nullptr;
        addButton_ = nullptr;
        itemEditor_ = nullptr;
        priorityButton_ = nullptr;
        dueDatePicker_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window_, message, wp, lp);
}
