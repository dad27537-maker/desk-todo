[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean,
    [string[]]$Targets = @('DeskTodo.Next'),
    [string]$BuildDirectory = 'DeskTodo.Next\build'
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $root 'scripts\BuildTools.ps1')
$cmake = Get-DeskTodoCMake
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
& $cmake -S (Join-Path $root 'DeskTodo.Next') -B $buildPath
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }
$arguments = @('--build', $buildPath, '--config', $Configuration, '--target') + $Targets
if ($Clean) { $arguments += '--clean-first' }
& $cmake @arguments -- /m
if ($LASTEXITCODE -ne 0) { throw '构建失败，请查看上方编译错误；如果程序正在运行，请先正常退出。' }

