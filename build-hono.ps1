#!/usr/bin/env pwsh
# Build the Hono framework bundle from npm source.
# Run this after `setup-vcpkg.ps1` if you want to update the embedded Hono.

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$honoDir = Join-Path $scriptDir "third_party/hono"
$honoBundle = Join-Path $scriptDir "src/js/hono.bundle.js"

# Install hono if not already present
if (-not (Test-Path (Join-Path $honoDir "node_modules/hono"))) {
    Write-Host "Installing hono..."
    Push-Location $honoDir
    npm install hono@4.12.30 2>&1 | Select-Object -Last 3
    Pop-Location
}

# Install esbuild if not already present
if (-not (Test-Path (Join-Path $honoDir "node_modules/esbuild"))) {
    Write-Host "Installing esbuild..."
    Push-Location $honoDir
    npm install esbuild 2>&1 | Select-Object -Last 3
    Pop-Location
}

# Bundle hono
Write-Host "Bundling hono..."
Push-Location $honoDir
npx esbuild node_modules/hono/dist/index.js --bundle --format=esm --platform=neutral --outfile=hono.bundle.js 2>&1
Pop-Location

# Copy to src/js
Copy-Item (Join-Path $honoDir "hono.bundle.js") $honoBundle -Force
Write-Host "Generated $honoBundle"
Write-Host "Run build.ps1 to rebuild the project with the new Hono bundle."
