<#
.SYNOPSIS
Write the setup-host release workflow into an existing PSX project (Windows).

.DESCRIPTION
The logic is tools/generate_ci.py; this only finds Python and passes every
argument through, so there is one implementation to fix.

.EXAMPLE
powershell -File psxrecomp\tools\generate_ci.ps1                 # cwd is the project
.EXAMPLE
powershell -File psxrecomp\tools\generate_ci.ps1 C:\src\MyGame --check
.EXAMPLE
powershell -File psxrecomp\tools\generate_ci.ps1 --force --zip-prefix mygame
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Passthrough = @()
)
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
if (-not $py) {
    Write-Error "generate_ci: Python 3 is not on PATH."
    exit 1
}
& $py.Source (Join-Path $ScriptDir "generate_ci.py") @Passthrough
exit $LASTEXITCODE
