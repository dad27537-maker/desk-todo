#pragma once
#include <string>
#include <vector>
#include "../Models/Fence.h"

std::wstring JsonEscape(const std::wstring& s);
std::wstring JsonSerialize(const std::vector<Fence>& fences);
bool JsonDeserialize(const std::wstring& json, std::vector<Fence>& outFences,
                     int* outVersion = nullptr);
