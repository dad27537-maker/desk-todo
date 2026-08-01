#pragma once

#include <Windows.h>
#include "Components/BlackHole/BlackHoleTypes.h"

namespace BlackHoleSettingsWindow {

BlackHole::Settings Load();
void Show(HWND owner);

}
