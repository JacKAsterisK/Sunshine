<#
.SYNOPSIS
Builds and installs this Sunshine fork on x64 Windows, or installs a copied build bundle.
.PARAMETER Action
Setup installs build prerequisites. Build packages the checkout. Install uses a local
bundle or builds the checkout. Update optionally pulls the current branch, builds, then installs.
.PARAMETER InstallerPath
An NSIS installer accompanied by Sunshine.install.json. Skips source building.
.PARAMETER NoPull
Rebuilds the current checkout without pulling when Action is Update.
.PARAMETER Help
Prints usage without changing the machine.
#>
param(
    [ValidateSet('Setup','Build','Install','Update')][string]$Action='Install',
    [string]$InstallerPath,
    [switch]$NoPull,
    [switch]$Help
)
$ErrorActionPreference='Stop'
$InformationPreference='Continue'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Invoke-CheckedCommand {
    <# .SYNOPSIS
    Runs a native command and refuses to continue after a nonzero exit status.
    #>
    param([string]$Command,[string[]]$Arguments)
    & $Command @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Command failed ($LASTEXITCODE)." }
}

function Invoke-SunshineShell {
    <# .SYNOPSIS
    Runs one synchronous command in a fresh MSYS2 UCRT64 shell.
    #>
    param([string]$Command)
    $root = 'C:\msys64'
    if ($env:SUNSHINE_MSYS2_ROOT) { $root = $env:SUNSHINE_MSYS2_ROOT }
    $shell = Join-Path $root 'msys2_shell.cmd'
    if (-not (Test-Path -LiteralPath $shell)) { throw 'MSYS2 is missing. Run setup.bat first.' }
    Invoke-CheckedCommand $shell @('-defterm','-here','-no-start','-ucrt64','-c',$Command)
}

function Get-NativeNodeDirectory {
    <# .SYNOPSIS
    Locates native Node.js, including a newly installed copy absent from this terminal PATH.
    #>
    $node = Get-Command node.exe -ErrorAction SilentlyContinue
    $path = if ($node) { $node.Source } else { Join-Path $env:ProgramFiles 'nodejs\node.exe' }
    if (-not (Test-Path -LiteralPath $path)) { throw 'Native Node.js is missing. Run setup.bat first.' }
    Invoke-CheckedCommand $path @('-e',"if (process.platform !== 'win32' || process.arch !== 'x64' || Number(process.versions.node.split('.')[0]) < 22) process.exit(1)")
    return Split-Path -Parent $path
}

function Invoke-SunshineSetup {
    <# .SYNOPSIS
    Installs missing MSYS2/Node and updates MSYS2 before installing the native build packages.
    #>
    $root = 'C:\msys64'
    if ($env:SUNSHINE_MSYS2_ROOT) { $root = $env:SUNSHINE_MSYS2_ROOT }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'msys2_shell.cmd'))) {
        Invoke-CheckedCommand 'winget.exe' @('install','--exact','--id','MSYS2.MSYS2','--source','winget',
            '--accept-package-agreements','--accept-source-agreements','--location',$root)
    }
    try { $null = Get-NativeNodeDirectory }
    catch {
        Invoke-CheckedCommand 'winget.exe' @('install','--exact','--id','OpenJS.NodeJS.LTS','--source','winget',
            '--accept-package-agreements','--accept-source-agreements')
        $null = Get-NativeNodeDirectory
    }
    Write-Information 'Updating MSYS2. If a core update closes its shell, rerun setup.bat.'
    Invoke-SunshineShell 'pacman -Syu --noconfirm'
    Invoke-SunshineShell 'pacman -Syu --noconfirm'
    Invoke-SunshineShell 'bash scripts/windows/setup.sh'
    Write-Information 'Build prerequisites are ready.'
}

function Invoke-SunshineBuild {
    <# .SYNOPSIS
    Produces the installer, a SHA-256 build receipt, and a deployable ZIP bundle.
    #>
    $env:SUNSHINE_NODE_DIR = Get-NativeNodeDirectory
    Invoke-SunshineShell 'bash scripts/windows/build.sh'
    $artifacts = Join-Path $repo 'cmake-build-windows\cpack_artifacts'
    $installer = Join-Path $artifacts 'Sunshine.exe'
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'CPack did not create the expected Sunshine.exe installer.' }
    $commit = & git.exe rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not read the source commit.' }
    $dirty = & git.exe status --porcelain --untracked-files=normal
    if ($LASTEXITCODE -ne 0) { throw 'Could not read the working-tree state.' }
    $receipt = [ordered]@{
        schema_version=1;architecture='x64';installer_name='Sunshine.exe'
        installer_sha256=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
        source_commit=$commit.Trim();source_dirty=[bool]$dirty;built_utc=[DateTimeOffset]::UtcNow.ToString('o')
    }
    $manifest = Join-Path $artifacts 'Sunshine.install.json'
    $receipt | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
    $bundle = Join-Path $repo 'cmake-build-windows\install-bundle'
    $null = New-Item -ItemType Directory -Path (Join-Path $bundle 'scripts\windows') -Force
    Copy-Item -LiteralPath $installer,$manifest -Destination $bundle -Force
    Copy-Item -LiteralPath (Join-Path $repo 'install.bat') -Destination $bundle -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sunshine-windows.ps1'),(Join-Path $PSScriptRoot 'SunshineInstall.psm1') -Destination (Join-Path $bundle 'scripts\windows') -Force
    Copy-Item -LiteralPath (Join-Path $repo 'docs\windows-fork-install.md') -Destination (Join-Path $bundle 'README.md') -Force
    $zip = Join-Path $artifacts 'Sunshine-fork-windows.zip'
    Compress-Archive -Path (Join-Path $bundle 'Sunshine.exe'),(Join-Path $bundle 'Sunshine.install.json'),(Join-Path $bundle 'install.bat'),(Join-Path $bundle 'scripts'),(Join-Path $bundle 'README.md') -DestinationPath $zip -Force
    Write-Information "Deployment bundle: $zip"
    return $installer
}

try {
    if ($Help) {
        Write-Information 'setup.bat                   Install MSYS2/UCRT64 and native Node build prerequisites'
        Write-Information 'build.bat                   Build this checkout and create a deployable installer bundle'
        Write-Information 'install.bat                 Install an adjacent bundle, or build and install this checkout'
        Write-Information 'install.bat -InstallerPath C:\path\Sunshine.exe    Install an existing fork build receipt/package'
        Write-Information 'update.bat                  Pull the clean current branch, build, then update Sunshine'
        Write-Information 'update.bat -NoPull           Build and update without pulling'
        Write-Information 'Use -Help for usage. Set SUNSHINE_NO_PAUSE=1 for automation.'
        exit 0
    }
    if ([Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITEW6432') -eq 'ARM64' -or
        ([Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE') -ne 'AMD64' -and
         [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITEW6432') -ne 'AMD64')) {
        throw 'These batch entrypoints support x64 Windows only. Use the documented CLANGARM64 build for ARM64.'
    }
    if ($InstallerPath -and $Action -notin @('Install')) { throw '-InstallerPath is only valid with install.bat.' }
    if ($NoPull -and $Action -ne 'Update') { throw '-NoPull is only valid with update.bat.' }
    Set-Location -LiteralPath $repo
    if ($Action -eq 'Setup') { Invoke-SunshineSetup; exit 0 }
    if ($Action -eq 'Update' -and -not $NoPull) {
        $dirty = & git.exe status --porcelain --untracked-files=normal
        if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Source update requires a clean checkout. Keep local changes and use update.bat -NoPull, or resolve them first.' }
        Invoke-CheckedCommand 'git.exe' @('pull','--ff-only')
    }
    if ($Action -eq 'Build') { $null = Invoke-SunshineBuild; exit 0 }
    if (-not $InstallerPath) {
        $adjacent = Join-Path $repo 'Sunshine.exe'
        if ($Action -eq 'Install' -and (Test-Path -LiteralPath $adjacent -PathType Leaf)) {
            $InstallerPath = $adjacent
        } else {
            $InstallerPath = Invoke-SunshineBuild
        }
    }
    Import-Module (Join-Path $PSScriptRoot 'SunshineInstall.psm1') -Force
    $receipt = Read-SunshineInstallerManifest -InstallerPath $InstallerPath
    $InstallerPath = (Get-Item -LiteralPath $InstallerPath).FullName
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Action Install -InstallerPath "' + $InstallerPath + '"'
        $child = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru -WindowStyle Hidden
        if ($child.ExitCode -ne 0) {
            throw "Elevated installation failed ($($child.ExitCode)). Run install.bat from an administrator terminal for details; inspect the installation and protected backups before retrying."
        }
        Write-Information "Installed fork build $($receipt.source_commit.Substring(0,8)). Finish setup at https://localhost:47990 on the host."
        Write-Information 'Configuration recovery copies, when applicable: %LOCALAPPDATA%\SunshineForkBackups'
        exit $child.ExitCode
    }
    Invoke-SunshinePackageInstall -InstallerPath $InstallerPath
    exit 0
} catch {
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}
