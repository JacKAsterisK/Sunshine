Set-StrictMode -Version Latest
$InformationPreference='Continue'

function Read-SunshineInstallerManifest {
    <# .SYNOPSIS
    Checks the local build receipt and installer hash before any host changes.
    #>
    param([Parameter(Mandatory=$true)][string]$InstallerPath)

    $installer = Get-Item -LiteralPath $InstallerPath -ErrorAction Stop
    if ($installer.PSIsContainer -or $installer.Extension -ine '.exe') {
        throw 'Supply the NSIS Sunshine.exe installer produced by build.bat.'
    }
    $manifestPath = Join-Path $installer.DirectoryName 'Sunshine.install.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or $manifest.architecture -ne 'x64' -or
        $manifest.installer_name -ne $installer.Name -or
        $manifest.installer_sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
        $manifest.source_commit -notmatch '^[a-fA-F0-9]{40}$') {
        throw 'Invalid Sunshine build receipt. Rebuild or recopy the complete installer bundle.'
    }
    if ((Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash -ine $manifest.installer_sha256) {
        throw 'Installer hash does not match the build receipt. No installation was attempted.'
    }
    return $manifest
}

function Get-SunshineInstallRoot {
    <# .SYNOPSIS
    Finds the installed service location, falling back to the standard install path.
    #>
    $service = Get-CimInstance Win32_Service -Filter "Name='SunshineService'" -ErrorAction Stop
    if ($service) {
        $path = $service.PathName
        if ($path -match '^"([^"\r\n]+\\tools\\sunshinesvc\.exe)"(?:\s|$)' -or
            $path -match '^([^"\r\n]+\\tools\\sunshinesvc\.exe)$') {
            return Split-Path -Parent (Split-Path -Parent $matches[1])
        }
        throw 'The existing Sunshine service has an unrecognized executable path. Inspect it before updating.'
    }
    return Join-Path $env:ProgramFiles 'Sunshine'
}

function Assert-SunshineInstallerCompatibility {
    <# .SYNOPSIS
    Refuses to overlay an MSI product with the NSIS installer.
    #>
    foreach ($path in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
        foreach ($key in @(Get-ChildItem -LiteralPath $path -ErrorAction SilentlyContinue)) {
            $product = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction Stop
            $name = $product.PSObject.Properties['DisplayName']
            $msi = $product.PSObject.Properties['WindowsInstaller']
            if ($name -and $name.Value -match '^Sunshine(?:\s|$)' -and $msi -and $msi.Value -eq 1) {
                throw 'Sunshine is installed through MSI. Keep its configuration and use its MSI upgrade/uninstall path before switching to NSIS.'
            }
        }
    }
}

function New-SunshineConfigurationBackup {
    <# .SYNOPSIS
    Copies configuration, pairing state and credentials to an owner/SYSTEM-only backup.
    #>
    [CmdletBinding(SupportsShouldProcess=$true)]
    param([Parameter(Mandatory=$true)][string]$InstallRoot)

    $paths = @('config','apps.json','sunshine.conf','sunshine_state.json','credentials','covers') |
        Where-Object { Test-Path -LiteralPath (Join-Path $InstallRoot $_) }
    if (-not $paths) { return $null }
    if (-not $PSCmdlet.ShouldProcess($InstallRoot,'Back up Sunshine configuration')) { return $null }
    $parent = Join-Path $env:LOCALAPPDATA 'SunshineForkBackups'
    $backup = Join-Path $parent ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $backup -Force
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.SetOwner($identity)
    foreach ($sid in @($identity,[Security.Principal.SecurityIdentifier]::new('S-1-5-18'))) {
        $rule = [Security.AccessControl.FileSystemAccessRule]::new(
            $sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $backup -AclObject $acl -ErrorAction Stop
    foreach ($relative in $paths) {
        $source = Get-Item -LiteralPath (Join-Path $InstallRoot $relative) -Force
        $entries = @($source)
        if ($source.PSIsContainer) { $entries += @(Get-ChildItem -LiteralPath $source.FullName -Recurse -Force) }
        if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
            throw 'Configuration backup contains a link. Inspect it before updating.'
        }
        Copy-Item -LiteralPath $source.FullName -Destination $backup -Recurse -Force -ErrorAction Stop
    }
    return $backup
}

function Stop-SunshineForInstall {
    <# .SYNOPSIS
    Stops Sunshine services and desktop instances only after the package is ready.
    #>
    [CmdletBinding(SupportsShouldProcess=$true)]
    param()
    if (-not $PSCmdlet.ShouldProcess('local Sunshine services and processes','Stop before installation')) { return }
    foreach ($name in @('SunshineService','sunshinesvc')) {
        $service = Get-Service -Name $name -ErrorAction SilentlyContinue
        if ($service -and $service.Status -ne 'Stopped') {
            Stop-Service -InputObject $service -Force -ErrorAction Stop
            $service.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20))
        }
    }
    foreach ($process in @(Get-Process -Name sunshine,sunshinesvc -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
        if (-not $process.WaitForExit(10000)) { throw 'A Sunshine process did not exit. Close it before retrying.' }
    }
}

function Invoke-SunshinePackageInstall {
    <# .SYNOPSIS
    Installs a verified NSIS package and checks the resulting executable and service.
    .DESCRIPTION
    Requires an elevated caller. Keeps configuration in place and creates a recovery copy.
    Does not automatically retry or uninstall a failed installation.
    #>
    [CmdletBinding(SupportsShouldProcess=$true)]
    param([Parameter(Mandatory=$true)][string]$InstallerPath)

    $manifest = Read-SunshineInstallerManifest -InstallerPath $InstallerPath
    Assert-SunshineInstallerCompatibility
    $root = Get-SunshineInstallRoot
    if (-not $PSCmdlet.ShouldProcess($root,'Back up, stop and install Sunshine')) { return }
    $backup = New-SunshineConfigurationBackup -InstallRoot $root
    if ($backup) { Write-Information "Configuration backup: $backup" }
    Stop-SunshineForInstall
    $installer = (Get-Item -LiteralPath $InstallerPath).FullName
    # NSIS requires /D to be the last argument, without quotes even when it contains spaces.
    $process = Start-Process -FilePath $installer -ArgumentList "/S /D=$root" -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "Sunshine installer failed with exit code $($process.ExitCode). Inspect the installation and backup before retrying."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'sunshine.exe') -PathType Leaf)) {
        throw 'Installer completed without the expected Sunshine executable. Inspect the installation before retrying.'
    }
    if ((Get-SunshineInstallRoot) -ine $root) { throw 'Installed Sunshine service does not point to the expected installation.' }
    $service = Get-Service -Name SunshineService -ErrorAction Stop
    if ($service.Status -ne 'Running') {
        Start-Service -InputObject $service -ErrorAction Stop
        $service.WaitForStatus('Running',[TimeSpan]::FromSeconds(20))
    }
    # The installer retains config on a silent upgrade; refuse a false success if it disappeared.
    if ($backup) {
        foreach ($item in @(Get-ChildItem -LiteralPath $backup -File -Recurse -Force)) {
            $relative = $item.FullName.Substring($backup.Length + 1)
            if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
                throw "An existing configuration file is missing after installation. Recovery copy: $backup"
            }
        }
    }
    Write-Information "Installed Sunshine fork build $($manifest.source_commit.Substring(0,8)) at $root"
    Write-Information 'Complete initial credentials and pairing at https://localhost:47990 on the host.'
}

Export-ModuleMember -Function Read-SunshineInstallerManifest,Get-SunshineInstallRoot,Assert-SunshineInstallerCompatibility,New-SunshineConfigurationBackup,Stop-SunshineForInstall,Invoke-SunshinePackageInstall
