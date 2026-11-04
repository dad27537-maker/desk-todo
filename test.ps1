[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [string]$BuildDirectory = 'DeskTodo.Next\build'
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $root 'scripts\BuildTools.ps1')
$cmake = Get-DeskTodoCMake
$targets = @('DeskTodoNativeSmoke', 'DeskTodoAsyncFileMoveSmoke', 'DeskTodoFenceRendererSmoke',
    'DeskTodoLogicalFenceSmoke', 'DeskTodoIconCacheSmoke', 'DeskTodoOleFileMoveSmoke',
    'DeskTodoOcrSmoke', 'DeskTodoOcrResultSmoke', 'DeskTodoTranslationSmoke',
    'DeskTodoTranslationWindowSmoke', 'DeskTodoScreenshotOcrSmoke', 'DeskTodoLanTransferSmoke',
    'DeskTodoBlackHoleSmoke', 'DeskTodoWidgetSmoke', 'DeskTodoBlackHoleRenderSmoke')
if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -Configuration $Configuration -Targets $targets -BuildDirectory $BuildDirectory
}
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
& $ctest --test-dir $buildPath -C $Configuration --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

