function Get-DeskTodoCMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $locator) {
        $found = & $locator -latest -products '*' -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if ($found) { return @($found)[0] }
    }
    throw '未找到 CMake。请安装 Visual Studio 的“使用 C++ 的桌面开发”和 CMake 工具。'
}
