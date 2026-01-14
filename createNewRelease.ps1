param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$releaseVersion,
    [switch]$ignoreLowerVersionCheck
)

function Convert-VersionToNumber($version) {
    $version = $version -replace '^v', ''
    $parts = $version.Split('.')
    return [int]$parts[0] * 1000000 + [int]$parts[1] * 1000 + [int]$parts[2]
}


# is semantic versioning valid?
if ($releaseVersion -notmatch '^v?\d{1,3}\.\d{1,3}\.\d{1,3}$') {
    throw "Invalid version format. Please use semantic versioning (e.g., 1.0.0)."
}

if($releaseVersion -notmatch '^v') {
    $releaseVersion = "v$releaseVersion"
}

# refresh git tags
git fetch --tags
if ($LASTEXITCODE -ne 0) {
    throw "Failed to fetch git tags."
}

# get git tags
$tags = (git tag) -split "`n" | Where-Object { $_.Trim() -ne "" -and $_ -match '^v?\d{1,3}\.\d{1,3}\.\d{1,3}' } | Sort-Object -Descending
if ($LASTEXITCODE -ne 0) {
    throw "Failed to retrieve git tags."
}

if($tags.Count -gt 0) {
    if($releaseVersion -in $tags) {
        throw "Release version $releaseVersion already exists."
    }
    $latestTag = $tags[0]
    if(-not $ignoreLowerVersionCheck) {
        # compare versions
        if ((Convert-VersionToNumber $releaseVersion) -le (Convert-VersionToNumber $latestTag)) {
            Write-Warning "Latest tag in repository is $latestTag, but release version is $releaseVersion.`nIf you are sure you want to proceed with a lower or equal version, use the -ignoreLowerVersionCheck switch."
            throw "Release version $releaseVersion must be greater than the latest version $latestTag."
        }
    }
    else {
        Write-Warning "Proceeding without version comparison due to -ignoreLowerVersionCheck switch."
    }
}
else {
    Write-Host "No existing tags found in the repository."
}

Write-Host "Release version $releaseVersion is valid and can be created."

Write-Host "Building frontendbuild.zip for release $releaseVersion..."
./justbuildwasm.ps1 -AsZipFile
Write-Host "Building Docker image backend for release $releaseVersion..."
./buildAndRun.ps1 -image backend -NoCache -PullLatestBaseImage -JustBuild
Write-Host "Building Docker image frontend for release $releaseVersion..."
./buildAndRun.ps1 -image frontend -NoCache -PullLatestBaseImage -JustBuild

function Tag-DockerImage {
    param (
        [string]$imageName,
        [string]$tag
    )
    Write-Host "Tagging Docker image $imageName with tag $tag..."
    docker tag $imageName $tag
    if($LASTEXITCODE -ne 0) {
        throw "Failed to tag Docker image $imageName with tag $tag."
    }
}
Tag-DockerImage rdp-backend:latest qxsch/freerdpweb-backend:latest
Tag-DockerImage rdp-backend:latest qxsch/freerdpweb-backend:$releaseVersion
Tag-DockerImage rdp-frontend:latest qxsch/freerdpweb-frontend:latest
Tag-DockerImage rdp-frontend:latest qxsch/freerdpweb-frontend:$releaseVersion

function Push-DockerImage {
    param(
        [string]$imageTag
    )
    Write-Host "Pushing Docker image $imageTag..."
    docker push $imageTag
    if($LASTEXITCODE -ne 0) {
        throw "Failed to push Docker image $imageTag."
    }
}

Push-DockerImage qxsch/freerdpweb-backend:latest
Push-DockerImage qxsch/freerdpweb-backend:$releaseVersion
Push-DockerImage qxsch/freerdpweb-frontend:latest
Push-DockerImage qxsch/freerdpweb-frontend:$releaseVersion


# create the git tag
git tag $releaseVersion
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create git tag $releaseVersion."
}
Write-Host "Git tag $releaseVersion created successfully."

# push the git tag
git push origin $releaseVersion
if ($LASTEXITCODE -ne 0) {
    throw "Failed to push git tag $releaseVersion."
}
Write-Host "Git tag $releaseVersion pushed successfully."

Write-Host -ForegroundColor Green "Release $releaseVersion created successfully."
