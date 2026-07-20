$ErrorActionPreference = "Stop"

# Enter the Visual Studio 2022 Developer Shell so cl.exe and the MSVC toolchain
# are on PATH. This allows CMake + Ninja to find the compiler without relying on
# a manually launched VS prompt.
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Error "vswhere.exe not found. Please install Visual Studio 2022."
}

$vsPath = & $vsWhere -latest -property installationPath
Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch amd64

$vcpkgRoot = Join-Path $PSScriptRoot "vcpkg"
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$vcpkgInstalled = Join-Path $PSScriptRoot "vcpkg_installed"
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"

if (-not (Test-Path $vcpkgExe)) {
    Write-Error "vcpkg not found at $vcpkgExe. Please run .\setup-vcpkg.ps1 first."
}

cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DCMAKE_TOOLCHAIN_FILE="$vcpkgToolchain" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DVCPKG_INSTALLED_DIR="$vcpkgInstalled"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build build -j
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Copy-Item -Force build\compile_commands.json compile_commands.json

Write-Host ""
Write-Host "Build completed at $(Get-Date)"
Write-Host "OK: build\asio_qjs.exe"
Write-Host "Run: .\build\asio_qjs.exe demo.js"
exit $LASTEXITCODE
