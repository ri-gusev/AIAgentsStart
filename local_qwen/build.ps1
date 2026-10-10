$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & g++ -std=c++17 -Wall -Wextra main.cpp agent.cpp -o local_qwen.exe -lcurl
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
} finally {
    Pop-Location
}
