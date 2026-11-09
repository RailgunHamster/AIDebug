#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string[]]$Engine,[string]$LegacyTools)
$ErrorActionPreference='Stop'
$package=(Resolve-Path "$PSScriptRoot/..").Path
$output=Join-Path $package ('Art/build/compatibility/'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $output|Out-Null
function Xml([string]$Value){[Security.SecurityElement]::Escape($Value)}
$targets=[Collections.Generic.List[object]]::new()
foreach($directory in $Engine){$targets.Add(@{engine=(Resolve-Path -LiteralPath $directory).Path;legacy=$false})}
if($LegacyTools){$targets.Add(@{engine=$targets[0].engine;legacy=$true})}
$index=0
foreach($target in $targets){
    $refs=@{}
    foreach($file in Get-ChildItem -LiteralPath "$($target.engine)/bin/managed" -File -Filter '*.dll'){
        if($file.Name -like 'System.*' -or $file.Name -in @('netstandard.dll','mscorlib.dll')){continue}
        $path=$file.FullName
        if($target.legacy -and (Test-Path -LiteralPath (Join-Path $LegacyTools $file.Name))){$path=Join-Path $LegacyTools $file.Name}
        $refs[$file.BaseName]=$path
    }
    foreach($file in Get-ChildItem -LiteralPath "$package/Editor/AIDebug/Managed" -File -Filter '*.dll'){
        if($file.BaseName -ne 'AIDebug.Sbox' -and !$refs.ContainsKey($file.BaseName)){$refs[$file.BaseName]=$file.FullName}
    }
    $references=foreach($name in $refs.Keys){'<Reference Include="'+(Xml $name)+'"><HintPath>'+(Xml $refs[$name])+'</HintPath><Private>false</Private></Reference>'}
    foreach($kind in @('Editor','Runtime')){
        $build=Join-Path $output ('compile-'+(++$index));New-Item -ItemType Directory -Force -Path $build|Out-Null
        $source=if($kind -eq 'Editor'){"$package/Editor/AIDebug/*.cs"}else{"$package/Runtime/*.cs"}
        @"
<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems><ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable><AllowUnsafeBlocks>true</AllowUnsafeBlocks><AssemblyName>AIDebug.Compatibility.$index</AssemblyName></PropertyGroup><ItemGroup>
<Using Include="Sandbox.Internal.GlobalToolsNamespace" Static="true" /><Using Include="Sandbox.Internal.GlobalGameNamespace" Static="true" />
$($references -join [Environment]::NewLine)
<Compile Include="$(Xml $source)" />
</ItemGroup></Project>
"@|Set-Content -LiteralPath "$build/test.csproj" -Encoding utf8
        & dotnet build "$build/test.csproj" --nologo -v minimal
        if($LASTEXITCODE){throw "$kind compilation failed: $($target.engine); legacy overrides: $($target.legacy)"}
        Write-Output "PASS $kind compile: $($target.engine); legacy overrides: $($target.legacy)"
    }
}
Write-Output "Passed $index full SDK compilations. Artifacts: $output"
