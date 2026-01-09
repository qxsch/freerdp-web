# current path of the script
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path


Set-Location "$scriptDir/frontend/progressive"
try {
    if(Test-Path build -PathType Container) {
        Write-Host "Removing existing build directory..."
        Remove-Item -Recurse -Force build
    }
    Write-Host "Building progressive decoder with Emscripten..."
    docker run --rm -v "${PWD}:/src" emscripten/emsdk:3.1.51 bash -c "cd /src && rm -rf build && mkdir build && bash build.sh"
}
finally {
    Set-Location $scriptDir
}


Set-Location "$scriptDir/frontend/clearcodec"
try {
    if(Test-Path build -PathType Container) {
        Write-Host "Removing existing build directory..."
        Remove-Item -Recurse -Force build
    }
    Write-Host "Building progressive decoder with Emscripten..."
    docker run --rm -v "${PWD}:/src" emscripten/emsdk:3.1.51 bash -c "cd /src && rm -rf build && mkdir build && bash build.sh"
}
finally {
    Set-Location $scriptDir
}
