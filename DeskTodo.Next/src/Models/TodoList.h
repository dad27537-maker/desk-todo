#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class TodoPriority : uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
};

enum class TodoSortMode : uint8_t {
    CompletedLast = 0,
    Priority = 1,
    DueDate = 2,
};

struct TodoItem {
    std::wstring text;
    bool completed = false;
    uint64_t id = 0;
    TodoPriority priority = TodoPriority::Normal;
    // ISO calendar date (YYYY-MM-DD). Empty means that no due date is set.
    std::wstring dueDate;
};

namespace TodoList {

inline constexpr size_t MaxItems = 100;
inline constexpr size_t MaxTextLength = 4096;

inline bool IsValidDueDate(const std::wstring& date) {
    if (date.empty()) return true;
    if (date.size() != 10 || date[4] != L'-' || date[7] != L'-') return false;
    for (size_t index = 0; index < date.size(); ++index) {
        if (index == 4 || index == 7) continue;
        if (date[index] < L'0' || date[index] > L'9') return false;
    }
    const int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day = std::stoi(date.substr(8, 2));
    if (year < 1601 || month < 1 || month > 12 || day < 1) return false;
    static constexpr int DaysPerMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int maximumDay = DaysPerMonth[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) maximumDay = 29;
    return day <= maximumDay;
}

inline uint64_t NextId(const std::vector<TodoItem>& items) {
    uint64_t maximum = 0;
    for (const auto& item : items) maximum = (std::max)(maximum, item.id);
    return maximum == UINT64_MAX ? 1 : maximum + 1;
}

inline bool Add(std::vector<TodoItem>& items, const std::wstring& text,
                TodoPriority priority = TodoPriority::Normal,
                const std::wstring& dueDate = L"") {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos || items.size() >= MaxItems ||
        !IsValidDueDate(dueDate)) return false;
    const size_t last = text.find_last_not_of(L" \t\r\n");
    std::wstring trimmed = text.substr(first, last - first + 1);
    if (trimmed.size() > MaxTextLength) return false;
    TodoItem item;
    item.text = std::move(trimmed);
    item.id = NextId(items);
    item.priority = priority;
    item.dueDate = dueDate;
    items.push_back(std::move(item));
    return true;
}

inline bool Toggle(std::vector<TodoItem>& items, size_t index) {
    if (index >= items.size()) return false;
    items[index].completed = !items[index].completed;
    return true;
}

inline bool Update(std::vector<TodoItem>& items, size_t index,
                   const std::wstring& text) {
    if (index >= items.size()) return false;
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const size_t last = text.find_last_not_of(L" \t\r\n");
    std::wstring trimmed = text.substr(first, last - first + 1);
    if (trimmed.size() > MaxTextLength) return false;
    items[index].text = std::move(trimmed);
    return true;
}

inline bool Remove(std::vector<TodoItem>& items, size_t index) {
    if (index >= items.size()) return false;
    items.erase(items.begin() + static_cast<ptrdiff_t>(index));
    return true;
}

inline int Remaining(const std::vector<TodoItem>& items) {
    int count = 0;
    for (const auto& item : items) if (!item.completed) ++count;
    return count;
}

inline int PriorityRank(TodoPriority priority) {
    switch (priority) {
    case TodoPriority::High: return 0;
    case TodoPriority::Normal: return 1;
    case TodoPriority::Low: return 2;
    }
    return 1;
}

inline void Sort(std::vector<TodoItem>& items, TodoSortMode mode) {
    std::stable_sort(items.begin(), items.end(),
        [mode](const TodoItem& left, const TodoItem& right) {
            if (left.completed != right.completed) return !left.completed;
            if (mode == TodoSortMode::Priority) {
                const int leftRank = PriorityRank(left.priority);
                const int rightRank = PriorityRank(right.priority);
                if (leftRank != rightRank) return leftRank < rightRank;
            } else if (mode == TodoSortMode::DueDate) {
                const bool leftHasDate = !left.dueDate.empty();
                const bool rightHasDate = !right.dueDate.empty();
                if (leftHasDate != rightHasDate) return leftHasDate;
                if (leftHasDate && left.dueDate != right.dueDate) {
                    return left.dueDate < right.dueDate;
                }
            }
            return left.id < right.id;
        });
}

}
