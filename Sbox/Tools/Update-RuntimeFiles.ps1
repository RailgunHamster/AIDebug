#Requires -Version 7.0
# Maintainer-only: refresh the prebuilt dependency inventory after a verified package update.
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../Editor/AIDebug").Path
$files=[ordered]@{}
Get-ChildItem -LiteralPath "$root/Managed" -File -Filter '*.dll' |
    Where-Object Name -notin @('AIDebug.Sbox.dll','Microsoft.CodeAnalysis.dll','Microsoft.CodeAnalysis.CSharp.dll') |
    Sort-Object Name | ForEach-Object {$files['Managed/'+$_.Name]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
$files['Native/aidebug_native.dll']=(Get-FileHash -LiteralPath "$root/Native/aidebug_native.dll" -Algorithm SHA256).Hash
[ordered]@{schema=1;platform='Windows x64';files=$files}|ConvertTo-Json -Depth 5|
    Set-Content -LiteralPath "$root/RuntimeFiles.json" -Encoding utf8
