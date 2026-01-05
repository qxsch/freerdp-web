param(
    [ValidateSet("frontend", "backend")]
    [Parameter(Mandatory=$true, Position=0)]
    [string]$image = "",
    [switch]$NoCache,
    [switch]$JustRun
)

if($JustRun -and $NoCache) {
    throw "Cannot use -JustRun and -NoCache together."
}

# current path of the script
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if($image -eq "frontend") {
    Write-Host "$scriptDir/frontend"
    Set-Location "$scriptDir/frontend"
    Write-Host "Building and running frontend..."
    if(-not $JustRun) {
        try {
            if($NoCache) {
                docker rmi rdp-frontend:latest | Out-Null
                docker build --no-cache -t rdp-frontend .
                if($LASTEXITCODE -ne 0) {
                    throw "Docker build failed with exit code $LASTEXITCODE"
                }
            }
            else {
                docker build -t rdp-frontend .
                if($LASTEXITCODE -ne 0) {
                    throw "Docker build failed with exit code $LASTEXITCODE"
                }
            }
        }
        finally {
            Set-Location $scriptDir
        }
    }
    docker run --rm -it -p 8000:8000 rdp-frontend
}
elseif($image -eq "backend") {
    Set-Location "$scriptDir/backend"
    Write-Host "Building and running backend..."
    if(-not $JustRun) {
        try {
            if($NoCache) {
                docker rmi rdp-backend:latest | Out-Null
                docker build --no-cache -t rdp-backend .
                if($LASTEXITCODE -ne 0) {
                    throw "Docker build failed with exit code $LASTEXITCODE"
                }
            }
            else {
                docker build -t rdp-backend .
                if($LASTEXITCODE -ne 0) {
                    throw "Docker build failed with exit code $LASTEXITCODE"
                }
            }
        }
        finally {
            Set-Location $scriptDir
        }
    }
    docker run --rm -it -p 8765:8765 rdp-backend
}
else {
    Write-Host "Please specify an image to build and run: -image frontend or -image backend"
}