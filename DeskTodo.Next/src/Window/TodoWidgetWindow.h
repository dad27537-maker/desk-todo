#pragma once

#include <Windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

#include "Models/TodoList.h"

class TodoWidgetWindow {
public:
    explicit TodoWidgetWindow(HINSTANCE instance);
    ~TodoWidgetWindow();

    void RestoreIfVisible();
    void ShowOrCreate();
    void Destroy();
    HWND Handle() const { return window_; }

private:
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND input_ = nullptr;
    HWND addButton_ = nullptr;
    HWND itemEditor_ = nullptr;
    HWND priorityButton_ = nullptr;
    HWND dueDatePicker_ = nullptr;
    HFONT inputFont_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    std::vector<TodoItem> items_;
    RECT savedRect_{};
    bool shouldRestore_ = false;
    bool dragging_ = false;
    POINT dragStart_{};
    RECT dragWindowStart_{};
    int firstVisible_ = 0;
    int editingIndex_ = -1;
    TodoPriority inputPriority_ = TodoPriority::Normal;
    TodoSortMode sortMode_ = TodoSortMode::CompletedLast;

    bool EnsureWindow();
    void LoadState();
    void SaveState(bool visible);
    void AddInputTask();
    void BeginEditTask(int index, int visibleRow);
    void EndEditTask(bool commit);
    void LayoutControls();
    void Paint(HDC dc);
    void ClampScroll();
    void ApplySort();
    void CycleSortMode();
    void CycleInputPriority();
    void ResetInputMetadata();
    std::wstring ReadDueDate() const;
    void SetDueDate(const std::wstring& dueDate);
    void ShowTaskContextMenu(int index, POINT screenPoint);
    int VisibleRowCount() const;
    RECT AddButtonRect() const;
    RECT CloseButtonRect() const;
    RECT SortButtonRect() const;
    RECT PriorityButtonRect() const;
    RECT DueDatePickerRect() const;
    RECT RowRect(int visibleRow) const;

    static LRESULT CALLBACK StaticWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditSubclassProc(HWND, UINT, WPARAM, LPARAM,
                                             UINT_PTR, DWORD_PTR);
    LRESULT WindowProc(UINT, WPARAM, LPARAM);
};
