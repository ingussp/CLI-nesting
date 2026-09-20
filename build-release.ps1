$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
  cmake --preset windows-release
  if ($LASTEXITCODE) { throw 'CMake configure failed' }
  cmake --build --preset windows-release --parallel
  if ($LASTEXITCODE) { throw 'Release build failed' }
  Write-Host "Executable: $PSScriptRoot\build-release\Release\clinesting.exe"
} finally { Pop-Location }
