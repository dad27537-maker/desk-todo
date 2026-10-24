cmake_minimum_required(VERSION 3.15)
project(DeskTodo.Next VERSION 1.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
add_compile_options("/utf-8" "/Zc:preprocessor")

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_NOMINMAX -D_CRT_SECURE_NO_WARNINGS)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h" "src/*.rc")
list(FILTER SOURCES EXCLUDE REGEX "Components/BlackHole/(BlackHoleComponent|D3D11BlackHoleBackend)\\.cpp$")
add_executable(DeskTodo.Next WIN32 ${SOURCES})
set(GHOSTTY_BH_ROOT "${CMAKE_SOURCE_DIR}/../reference_repos/ghostty-blackhole-main-old")
target_sources(DeskTodo.Next PRIVATE
    "${GHOSTTY_BH_ROOT}/src/capture_wgc.cpp"
    "${GHOSTTY_BH_ROOT}/src/capture_dxgi.cpp"
    "${GHOSTTY_BH_ROOT}/src/gl_texture.cpp"
    "${GHOSTTY_BH_ROOT}/src/win32_gl.cpp"
    "${GHOSTTY_BH_ROOT}/src/monitors.cpp")
target_include_directories(DeskTodo.Next PRIVATE src
    "src/Components/BlackHole/GhosttyCompat"
    "${GHOSTTY_BH_ROOT}/src")
if(MSVC)
    target_compile_options(DeskTodo.Next PRIVATE "/FIWindows.h")
    target_link_options(DeskTodo.Next PRIVATE
        "/MANIFEST:NO"
        "/DELAYLOAD:d2d1.dll"
        "/DELAYLOAD:dwrite.dll")
endif()

target_link_libraries(DeskTodo.Next PRIVATE
    d2d1 dwrite shell32 shcore comctl32 comdlg32 gdi32 msimg32 ole32 oleaut32
    runtimeobject windowsapp windowscodecs user32 kernel32 advapi32 delayimp
    d3d11 dxgi d3dcompiler ws2_32 iphlpapi opengl32 dwmapi wtsapi32 avrt)

add_custom_command(TARGET DeskTodo.Next POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GHOSTTY_BH_ROOT}/blackhole.glsl" "$<TARGET_FILE_DIR:DeskTodo.Next>/blackhole.glsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GHOSTTY_BH_ROOT}/blackhole_presets.txt" "$<TARGET_FILE_DIR:DeskTodo.Next>/blackhole_presets.txt")

# Some revisions embed the shaders in C++ and do not ship this optional
# directory. Keep the post-build step valid for both source layouts.
if(EXISTS "${GHOSTTY_BH_ROOT}/shaders")
    add_custom_command(TARGET DeskTodo.Next POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GHOSTTY_BH_ROOT}/shaders" "$<TARGET_FILE_DIR:DeskTodo.Next>/shaders")
endif()

add_executable(DeskTodoNativeSmoke
    tests/native-smoke.cpp
    src/Services/FileMoveService.cpp
    src/Services/FileService.cpp
    src/Services/ConfigService.cpp
    src/Services/DesktopPathService.cpp
    src/Services/DesktopShellService.cpp
    src/Services/DesktopIconLayoutService.cpp
    src/Services/GridLayoutService.cpp
    src/Services/MonitorService.cpp
    src/Services/SnapManager.cpp
    src/Services/ShellIntegrationService.cpp
    src/Components/BlackHole/BlackHoleComponent.cpp
    src/Models/PomodoroTimer.h
    src/Models/TodoList.h
    src/Models/CaptureRegion.h
    src/Utils/JsonUtils.cpp)
target_include_directories(DeskTodoNativeSmoke PRIVATE src)
target_link_libraries(DeskTodoNativeSmoke PRIVATE
    shell32 shcore ole32 oleaut32 advapi32 user32 kernel32)

add_executable(DeskTodoLogicalFenceSmoke
    tests/logical-fence-smoke.cpp
    src/Managers/FenceManager.cpp
    src/Services/ConfigService.cpp
    src/Services/DesktopPathService.cpp
    src/Services/DesktopShellService.cpp
    src/Services/FenceStorageService.cpp
    src/Services/FileMoveService.cpp
    src/Services/FileService.cpp
    src/Services/GridLayoutService.cpp
    src/Services/IconCache.cpp
    src/Services/MonitorService.cpp
    src/Services/ShellIntegrationService.cpp
    src/Utils/JsonUtils.cpp)
target_include_directories(DeskTodoLogicalFenceSmoke PRIVATE src)
target_link_libraries(DeskTodoLogicalFenceSmoke PRIVATE
    d2d1 dwrite shell32 shcore comctl32 gdi32 ole32 oleaut32 windowscodecs
    user32 kernel32 advapi32)

add_executable(DeskTodoDesktopDragProbe
    tests/desktop-drag-probe.cpp
    src/Services/DesktopShellService.cpp
    src/Services/DesktopPathService.cpp
    src/Services/ShellIntegrationService.cpp)
target_include_directories(DeskTodoDesktopDragProbe PRIVATE src)
target_link_libraries(DeskTodoDesktopDragProbe PRIVATE
    shell32 comctl32 ole32 oleaut32 user32 kernel32)

add_executable(DeskTodoOcrSmoke
    tests/ocr-smoke.cpp
    src/Services/OcrService.cpp
    src/Services/PaddleOcrService.cpp)
target_include_directories(DeskTodoOcrSmoke PRIVATE src)
target_compile_definitions(DeskTodoOcrSmoke PRIVATE
    DESKTODO_OCR_FIXTURE_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(DeskTodoOcrSmoke PRIVATE
    gdi32 ole32 runtimeobject windowsapp windowscodecs user32 kernel32)

# setup-ocr.ps1 installs pinned offline OCR dependencies. Other builds retain
# the enhanced Windows OCR fallback.
set(OCR_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/deps/ocr")
set(ORT_INCLUDE "${OCR_DEPS}/onnxruntime-win-x64-1.31.0/include")
set(OPENCV_ROOT "${OCR_DEPS}/opencv/build")
if(EXISTS "${ORT_INCLUDE}/onnxruntime_cxx_api.h" AND
   EXISTS "${OPENCV_ROOT}/x64/vc16/lib/opencv_world4120.lib" AND
   EXISTS "${OCR_DEPS}/runtime/PP-OCRv6_rec_medium.onnx" AND
   EXISTS "${OCR_DEPS}/runtime/PP-OCRv6_det_medium.onnx")
    foreach(OCR_TARGET DeskTodo.Next DeskTodoOcrSmoke)
        target_compile_definitions(${OCR_TARGET} PRIVATE DESKTODO_HAS_PADDLE_OCR=1)
        target_include_directories(${OCR_TARGET} SYSTEM PRIVATE
            "${ORT_INCLUDE}" "${OPENCV_ROOT}/include")
        target_link_libraries(${OCR_TARGET} PRIVATE
            "${OPENCV_ROOT}/x64/vc16/lib/opencv_world4120.lib" delayimp)
        target_link_options(${OCR_TARGET} PRIVATE "/DELAYLOAD:opencv_world4120.dll")
        add_custom_command(TARGET ${OCR_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${OCR_DEPS}/runtime" "$<TARGET_FILE_DIR:${OCR_TARGET}>/ocr")
    endforeach()
    message(STATUS "Offline OCR: PP-OCRv6 medium with ONNX Runtime")
else()
    message(STATUS "Offline OCR models not installed; run DeskTodo.Next/setup-ocr.ps1 for PP-OCRv6")
endif()

add_executable(DeskTodoOcrResultSmoke
    tests/ocr-result-smoke.cpp
    src/Window/OcrResultWindow.cpp)
target_include_directories(DeskTodoOcrResultSmoke PRIVATE src)
target_link_libraries(DeskTodoOcrResultSmoke PRIVATE gdi32 user32 kernel32)

add_executable(DeskTodoScreenshotOcrSmoke
    tests/screenshot-ocr-smoke.cpp
    src/Window/ScreenshotTool.cpp
    src/Services/DesktopPathService.cpp)
target_include_directories(DeskTodoScreenshotOcrSmoke PRIVATE src)
target_link_libraries(DeskTodoScreenshotOcrSmoke PRIVATE
    gdi32 msimg32 comdlg32 comctl32 shell32 ole32 windowscodecs advapi32 user32 kernel32)

add_executable(DeskTodoLanTransferSmoke
    tests/lan-transfer-smoke.cpp
    src/Window/LanTransferWindow.cpp)
target_include_directories(DeskTodoLanTransferSmoke PRIVATE src)
target_link_libraries(DeskTodoLanTransferSmoke PRIVATE
    shell32 ole32 uuid gdi32 user32 ws2_32 iphlpapi)

add_executable(DeskTodoBlackHoleSmoke
    tests/black-hole-widget-smoke.cpp
    src/Components/BlackHole/BlackHoleBackground.cpp)
target_include_directories(DeskTodoBlackHoleSmoke PRIVATE src)
target_link_libraries(DeskTodoBlackHoleSmoke PRIVATE
    shell32 ole32 user32 kernel32)
