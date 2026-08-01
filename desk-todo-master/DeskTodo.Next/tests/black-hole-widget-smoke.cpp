#include <iostream>

#include "Components/BlackHole/BlackHoleBackground.h"
#include "Components/BlackHole/BlackHoleTypes.h"

int main() {
    BlackHole::Settings unsafe;
    unsafe.diskTemperature = 90000.0f;
    unsafe.diskOpacity = 4.0f;
    unsafe.diskInnerRadius = 9.8f;
    unsafe.diskOuterRadius = 2.0f;
    const auto safe = BlackHole::Settings::Sanitize(unsafe);
    const bool sanitized = safe.diskTemperature == 30000.0f &&
        safe.diskOpacity == 1.0f && safe.diskOuterRadius >= safe.diskInnerRadius + 0.5f;

    BlackHoleBackground controller;
    controller.UpdateSettings(safe);
    const auto roundTrip = controller.CurrentSettings();
    const bool stored = roundTrip.diskTemperature == safe.diskTemperature &&
        roundTrip.diskOuterRadius == safe.diskOuterRadius;
    const bool initiallyStopped = !controller.IsRunning();
    controller.Stop();

    const int failures = (!sanitized) + (!stored) + (!initiallyStopped);
    if (failures) {
        std::cerr << "DeskTodo black hole overlay checks: 3, failures: " << failures << "\n";
        return 1;
    }
    std::cout << "DeskTodo black hole overlay checks: 3, failures: 0\n";
    return 0;
}
