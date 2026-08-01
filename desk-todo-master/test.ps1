[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$testScript = Join-Path $root "DeskTodo.Native\tests\native-smoke.ps1"
$publishedExe = Join-Path $root "DeskTodo.exe"

& $testScript -Executable $publishedExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

