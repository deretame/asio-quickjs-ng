$ErrorActionPreference = "Stop"
$env:PATH = "D:\msys2\ucrt64\bin;D:\msys2\usr\bin;" + $env:PATH

cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=gcc `
  -DCMAKE_CXX_COMPILER=g++ `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j

Copy-Item -Force build\compile_commands.json compile_commands.json

Write-Host "OK: build\asio_qjs.exe"
Write-Host "Run: .\build\asio_qjs.exe demo.js"
