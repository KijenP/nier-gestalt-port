# Git for Windows without symlink support checks symlinks out as small text
# files holding the target path. Replace each one with a copy of its target.
param([Parameter(Mandatory)][string]$SdkRoot)

$git = (Get-Command git -ErrorAction SilentlyContinue).Source
if (-not $git) { throw 'git not found on PATH (run from scripts\env.cmd)' }

$repos = @('.') + (& $git -C $SdkRoot config --file .gitmodules --get-regexp 'path$' |
    ForEach-Object { ($_ -split ' ', 2)[1] })

function Test-UnresolvedLink([string]$path) {
    if ((Get-Item -LiteralPath $path).Length -gt 1024) { return $false }
    return (Get-Content -LiteralPath $path -Raw) -match '^[^\r\n]+\.\w+\s*$' -and
        (Get-Content -LiteralPath $path -Raw) -notmatch '[\s{};#]\S'
}

# Links can point at other links, so repeat until nothing changes.
do {
    $fixed = 0
    foreach ($repo in $repos) {
        $repoDir = Join-Path $SdkRoot $repo
        foreach ($entry in & $git -C $repoDir ls-files -s) {
            if ($entry -notmatch '^120000 \S+ \d+\t(.+)$') { continue }
            $link = Join-Path $repoDir $Matches[1]
            if (-not (Test-Path -LiteralPath $link -PathType Leaf)) { continue }
            if (-not (Test-UnresolvedLink $link)) { continue }  # already replaced
            $target = [IO.Path]::GetFullPath((Join-Path (Split-Path $link) (Get-Content -LiteralPath $link -Raw).Trim()))
            if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
                Write-Warning "skip (target missing or directory): $link -> $target"
                continue
            }
            if (Test-UnresolvedLink $target) { continue }  # resolve the target first
            Copy-Item -LiteralPath $target -Destination $link -Force
            & $git -C $repoDir update-index --assume-unchanged -- $Matches[1]
            $fixed++
        }
    }
    Write-Host "replaced $fixed link(s)"
} while ($fixed -gt 0)
