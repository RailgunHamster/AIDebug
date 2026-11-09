#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Engine,[string]$VsTools='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat')
$ErrorActionPreference='Stop'
$game=(Resolve-Path -LiteralPath $Engine).Path
if(!(Test-Path -LiteralPath "$game/bin/managed/Sandbox.Engine.dll")){$game=Join-Path $game 'game'}
if(!(Test-Path -LiteralPath "$game/bin/managed/Sandbox.Engine.dll")){throw 'Engine must contain bin/managed/Sandbox.Engine.dll'}
$package=(Resolve-Path "$PSScriptRoot/..").Path
& dotnet build "$package/Runtime/AIDebug.Sbox.csproj" -c Release "/p:EngineBin=$game/bin" --nologo
if($LASTEXITCODE -ne 0){throw 'Managed AIDebug build failed'}
$output=Join-Path $package 'Art/build/native'
$native=Join-Path $package 'Editor/AIDebug/Native'
New-Item -ItemType Directory -Force -Path $output,$native|Out-Null
$source=Join-Path $package 'Native'
$vendor=Join-Path $source 'thirdparty/minhook'
$commands=@"
@echo off
call "$VsTools" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /c /I"$vendor/include" "$vendor/src/buffer.c" "$vendor/src/hook.c" "$vendor/src/trampoline.c" "$vendor/src/hde/hde64.c" /Fo"$output\\"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /std:c++17 /EHsc /LD "$source/aidebug_native.cpp" "$output/buffer.obj" "$output/hook.obj" "$output/trampoline.obj" "$output/hde64.obj" /Fo"$output/native.obj" /link /Brepro /OUT:"$native/aidebug_native.dll" /IMPLIB:"$output/native.lib"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /LD "$package/Tests/native_fixture.c" /Fo"$output/fixture.obj" /link /Brepro /OUT:"$output/native_fixture.dll" /IMPLIB:"$output/fixture.lib"
"@
$commandFile=Join-Path $output 'build.cmd';[IO.File]::WriteAllText($commandFile,$commands)
& $env:ComSpec /d /c $commandFile
if($LASTEXITCODE -ne 0){throw 'Native AIDebug build failed'}
& "$PSScriptRoot/Update-RuntimeFiles.ps1"
Write-Output 'Built the independent s&box runtime debugger, native adapter and test fixture.'
