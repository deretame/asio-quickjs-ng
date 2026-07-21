#Requires -Version 5.1

<#
.SYNOPSIS
    Bootstrap a project-local vcpkg clone and install dependencies.

.DESCRIPTION
    Clones microsoft/vcpkg at the pinned tag, bootstraps vcpkg.exe if needed,
    and runs vcpkg install for x64-windows-static. This script is Windows-only
    and uses PowerShell-native constructs (no WSL, no MSYS, no bash).

.PARAMETER VcpkgVersion
    The vcpkg Git tag to checkout. Default: 2026.04.27.

.PARAMETER Force
    Force a fresh checkout/fetch even if the correct version is already present.
#>
[CmdletBinding()]
param(
    [string]$VcpkgVersion = "2026.04.27",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ========== 环境校验 ==========
if ($env:OS -ne "Windows_NT")
{
    throw "This script is Windows-only."
}

$git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $git)
{
    throw "Git is required but not found in PATH. Install Git for Windows and try again."
}

# 进入 VS Developer Shell，确保使用 Windows 原生 MSVC/CMake/Ninja，避免 MSYS2 工具污染 PATH
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere))
{
    throw "vswhere.exe not found. Please install Visual Studio with the C++ workload."
}

$vsPath = & $vsWhere `
    -latest `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath)
{
    throw "Visual Studio with C++ build tools not found."
}

Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch amd64

# 净化 PATH：移除 MSYS2/Cygwin/MinGW 路径，避免 vcpkg 调用 Unix 风格工具。
# git 使用启动前捕获的完整路径执行，其所在目录不再加回 PATH，防止 MSYS2 目录被重新置顶。
$gitExe = $git.Source
$unixToolNames = @(
    'msys2', 'msys64', 'msys32', 'msys',
    'cygwin64', 'cygwin32', 'cygwin',
    'mingw64', 'mingw32', 'mingw-w64', 'mingw'
)
$pathEntries = $env:PATH -split ";" | Where-Object {
    $entry = $_.Trim().Trim('"').Trim("'")
    if (-not $entry) { return $false }
    # 展开 %VAR% 形式的环境变量，防止 PATH 里写的是 %MSYS2_ROOT%\usr\bin
    $expanded = [Environment]::ExpandEnvironmentVariables($entry)
    $normalized = ($expanded.Replace('\', '/')).TrimEnd('/') + '/'
    $isBad = $false
    foreach ($name in $unixToolNames)
    {
        if ($normalized -like "*/$name/*" -or $normalized -like "*/$name")
        {
            $isBad = $true
            break
        }
    }
    -not $isBad
}
$env:PATH = ($pathEntries | Select-Object -Unique) -join ";"

# 如果 cmake 仍然解析到 MSYS2/Cygwin/MinGW，强制把 VS 自带的 CMake 目录置顶
$cmake = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
if ($cmake)
{
    $cmakeDir = Split-Path -Parent $cmake.Source
    if ($cmakeDir -replace '\\', '/' -match '/msys[0-9]*/|/cygwin[0-9]*/|/mingw(?:-w)?[0-9]*/')
    {
        Write-Warning "cmake still resolves to Unix-style path: $($cmake.Source)"
        $vsCMakeDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
        if (Test-Path $vsCMakeDir)
        {
            Write-Host "Prepending VS CMake directory: $vsCMakeDir"
            $env:PATH = "$vsCMakeDir;$env:PATH"
        }
        else
        {
            throw "Unable to locate Windows-native CMake. Remove MSYS2/Cygwin/MinGW from PATH and retry."
        }
    }
    $cmake = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
    Write-Host "Using cmake: $($cmake.Source)"
}
else
{
    Write-Warning "cmake not found in PATH after sanitization"
}

# ========== 路径与常量 ==========
$vcpkgRoot = Join-Path $PSScriptRoot "vcpkg"
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"

# 保存并覆盖 VCPKG_ROOT，确保使用项目本地 vcpkg
$oldVcpkgRoot = $env:VCPKG_ROOT
$env:VCPKG_ROOT = $vcpkgRoot

function Restore-VcpkgRoot
{
    if ($oldVcpkgRoot)
    {
        $env:VCPKG_ROOT = $oldVcpkgRoot
    }
    else
    {
        Remove-Item Env:VCPKG_ROOT -ErrorAction SilentlyContinue
    }
}

function Invoke-NativeCommand
{
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Command
    )

    & $Command
    if ($LASTEXITCODE -ne 0)
    {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command"
    }
}

# ========== 主流程 ==========
try
{
    Write-Host "=== Setting up vcpkg $VcpkgVersion (Windows native) ==="

    $gitDir = Join-Path $vcpkgRoot ".git"

    if (-not (Test-Path $gitDir))
    {
        if (Test-Path $vcpkgRoot)
        {
            Write-Host "Removing incomplete vcpkg directory ..."
            Remove-Item -Recurse -Force $vcpkgRoot
        }

        Write-Host "Cloning vcpkg $VcpkgVersion (shallow) ..."
        Invoke-NativeCommand { & $gitExe clone --depth 1 --branch $VcpkgVersion -- https://github.com/microsoft/vcpkg.git $vcpkgRoot }
    }
    else
    {
        Push-Location $vcpkgRoot
        try
        {
            if ($Force)
            {
                Write-Host "Force switch set; checking out vcpkg $VcpkgVersion ..."
                Invoke-NativeCommand { & $gitExe fetch --depth 1 --force origin tag $VcpkgVersion }
                Invoke-NativeCommand { & $gitExe checkout $VcpkgVersion }
            }
            else
            {
                # 比较 commit hash，避免无意义的网络拉取；对 shallow clone 也有效
                $targetCommit = & $gitExe rev-list -n 1 $VcpkgVersion 2>$null
                if ($LASTEXITCODE -eq 0)
                {
                    $currentCommit = & $gitExe rev-parse HEAD
                    if ($currentCommit -eq $targetCommit)
                    {
                        Write-Host "vcpkg already at $VcpkgVersion ($currentCommit)"
                    }
                    else
                    {
                        Write-Host "Switching vcpkg from $currentCommit to $VcpkgVersion ..."
                        Invoke-NativeCommand { & $gitExe fetch --depth 1 --force origin tag $VcpkgVersion }
                        Invoke-NativeCommand { & $gitExe checkout $VcpkgVersion }
                    }
                }
                else
                {
                    Write-Host "Fetching vcpkg $VcpkgVersion ..."
                    Invoke-NativeCommand { & $gitExe fetch --depth 1 --force origin tag $VcpkgVersion }
                    Invoke-NativeCommand { & $gitExe checkout $VcpkgVersion }
                }
            }
        }
        finally
        {
            Pop-Location
        }
    }

    Push-Location $vcpkgRoot
    try
    {
        if (-not (Test-Path $vcpkgExe))
        {
            Write-Host "Bootstrapping vcpkg ..."
            Invoke-NativeCommand { .\bootstrap-vcpkg.bat }
        }

        Write-Host "vcpkg version:"
        Invoke-NativeCommand { .\vcpkg.exe --version }

        Write-Host "`nInstalling dependencies for $env:VCPKG_DEFAULT_TRIPLET ..."
        Invoke-NativeCommand { .\vcpkg.exe install --triplet $env:VCPKG_DEFAULT_TRIPLET --no-print-usage }

        Write-Host "`n=== Setup complete ==="
        Write-Host "OK: vcpkg dependencies installed."
        Write-Host "Run: .\build.ps1"
    }
    finally
    {
        Pop-Location
    }
}
finally
{
    Restore-VcpkgRoot
}
