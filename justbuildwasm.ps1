# current path of the script
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path


# remove the directory wasmbuild if it exists
$wasmBuildDir = Join-Path $scriptDir "wasmbuild"
if(Test-Path $wasmBuildDir -PathType Container) {
    Write-Host "Removing existing wasmbuild directory..."
    Remove-Item -Recurse -Force $wasmBuildDir
}
# copy frontend to wasmbuild
Write-Host "Copying frontend to wasmbuild directory..."
Copy-Item -Recurse -Force (Join-Path $scriptDir "frontend") $wasmBuildDir



Set-Location (Join-Path $wasmBuildDir "progressive")
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


Set-Location (Join-Path $wasmBuildDir "clearcodec")
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




#remove build directories from wasmbuild/frontend
foreach($dir in Get-ChildItem -Path $wasmBuildDir -Recurse -Directory | Where-Object { $_.Name -eq "progressive" -or $_.Name -eq "clearcodec" }) {
    foreach($f in @("build", "build.sh", "CMakeLists.txt")) {
        $itemToRemove = Join-Path $dir.FullName $f
        if(Test-Path $itemToRemove) {
            Write-Host "Removing $f from $($dir.FullName)..."
            Remove-Item -Recurse -Force $itemToRemove
        }
    }
    # remove all .c and .h files
    Get-ChildItem -Path $dir.FullName -Filter *.c | ForEach-Object {
        Write-Host "Removing $($_.FullName)..."
        Remove-Item -Force $_.FullName
    }
    Get-ChildItem -Path $dir.FullName -Filter *.h | ForEach-Object {
        Write-Host "Removing $($_.FullName)..."
        Remove-Item -Force $_.FullName
    }
}


Remove-Item (Join-Path $wasmBuildDir "Dockerfile") -Force
Remove-Item (Join-Path $wasmBuildDir "nginx.conf") -Force

Write-Host -ForegroundColor Green "WASM build completed. Output is in the 'wasmbuild' directory."
Write-Host -ForegroundColor Green " - do not forget the set the appropriate headers for .wasm files when serving via HTTP."
Write-Host -ForegroundColor Green " - For SharedArrayBuffer support, also set the following headers:"
Write-Host -ForegroundColor Green "     -> Cross-Origin-Opener-Policy: same-origin"
Write-Host -ForegroundColor Green "     -> Cross-Origin-Embedder-Policy: require-corp"
