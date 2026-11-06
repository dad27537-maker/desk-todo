[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$nativeBuild = Join-Path $root "DeskTodo.Native\build.ps1"
$publish = Join-Path $root "DeskTodo.Native\publish.ps1"

& $nativeBuild -Configuration $Configuration -Clean:$Clean
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $publish

