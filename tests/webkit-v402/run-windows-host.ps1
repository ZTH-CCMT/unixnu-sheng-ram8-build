param(
    [string]$Source,
    [string]$Output,
    [switch]$GenerateOnly
)
$ErrorActionPreference = 'Stop'
$arguments = @((Join-Path $PSScriptRoot 'run-windows-host.py'))
if ($Source) { $arguments += @('--source', $Source) }
if ($Output) { $arguments += @('--output', $Output) }
if ($GenerateOnly) { $arguments += '--generate-only' }
& python @arguments
exit $LASTEXITCODE
