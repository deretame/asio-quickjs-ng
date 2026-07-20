$ErrorActionPreference = "Stop"

$vcpkgVersion = "2026.04.27"
$vcpkgRoot = Join-Path $PSScriptRoot "vcpkg"
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"

$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"

if (-not (Test-Path $vcpkgRoot)) {
    Write-Host "Cloning vcpkg $vcpkgVersion into $vcpkgRoot ..."
    git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}

Push-Location $vcpkgRoot
try {
    Write-Host "Checking out vcpkg $vcpkgVersion ..."
    git fetch --tags
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }
    git checkout $vcpkgVersion
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed" }

    if (-not (Test-Path $vcpkgExe)) {
        Write-Host "Bootstrapping vcpkg ..."
        .\bootstrap-vcpkg.bat
        if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
    }

    Write-Host "Installing dependencies for $env:VCPKG_DEFAULT_TRIPLET ..."
    .\vcpkg install
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }
}
finally {
    Pop-Location
}

Write-Host "OK: vcpkg dependencies installed."
Write-Host "Run: .\build.ps1"
