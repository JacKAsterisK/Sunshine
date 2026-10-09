BeforeDiscovery {
    $modulePath = Join-Path $PSScriptRoot '..\..\scripts\windows\SunshineInstall.psm1'
    Import-Module $modulePath -Force
}

Describe 'Fork installer integrity' {
    BeforeEach {
        $installer = Join-Path $TestDrive 'Sunshine.exe'
        Set-Content -LiteralPath $installer -Value 'fixture package'
        $manifestPath = Join-Path $TestDrive 'Sunshine.install.json'
        $manifest = @{
            schema_version=1;architecture='x64';installer_name='Sunshine.exe'
            source_commit=('a' * 40)
            installer_sha256=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
        }
        $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath
    }

    It 'accepts a package matching its receipt' {
        (Read-SunshineInstallerManifest $installer).source_commit | Should -Be ('a' * 40)
    }
    It 'refuses a changed package' {
        Add-Content -LiteralPath $installer -Value 'changed'
        { Read-SunshineInstallerManifest $installer } | Should -Throw '*hash*'
    }
    It 'refuses a missing receipt' {
        Remove-Item -LiteralPath $manifestPath
        { Read-SunshineInstallerManifest $installer } | Should -Throw
    }
    It 'refuses a receipt for another architecture' {
        $manifest.architecture='arm64'
        $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath
        { Read-SunshineInstallerManifest $installer } | Should -Throw '*receipt*'
    }
    It 'refuses another source identifier format' {
        $manifest.source_commit='not-a-commit'
        $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath
        { Read-SunshineInstallerManifest $installer } | Should -Throw '*receipt*'
    }
    It 'refuses a receipt for a differently named executable' {
        $manifest.installer_name='different.exe'
        $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath
        { Read-SunshineInstallerManifest $installer } | Should -Throw '*receipt*'
    }
}

Describe 'Installed service discovery' {
    InModuleScope SunshineInstall {
        It 'uses the standard root for a first installation' {
            Mock Get-CimInstance { $null }
            Get-SunshineInstallRoot | Should -Be (Join-Path $env:ProgramFiles 'Sunshine')
        }
        It 'preserves a custom service location containing spaces' {
            Mock Get-CimInstance { [pscustomobject]@{PathName='"D:\Streaming Apps\Sunshine\tools\sunshinesvc.exe"'} }
            Get-SunshineInstallRoot | Should -Be 'D:\Streaming Apps\Sunshine'
        }
        It 'rejects an ambiguous executable path before updating files' {
            Mock Get-CimInstance { [pscustomobject]@{PathName='"D:\Streaming Apps\different.exe" --argument'} }
            { Get-SunshineInstallRoot } | Should -Throw '*unrecognized*'
        }
        It 'refuses an existing MSI product' {
            Mock Get-ChildItem { [pscustomobject]@{PSPath='HKLM:\fixture'} }
            Mock Get-ItemProperty { [pscustomobject]@{DisplayName='Sunshine';WindowsInstaller=1} }
            { Assert-SunshineInstallerCompatibility } | Should -Throw '*MSI*'
        }
        It 'accepts a normal NSIS installation and unrelated MSI products' {
            Mock Get-ChildItem { [pscustomobject]@{PSPath='HKLM:\fixture'} }
            Mock Get-ItemProperty { [pscustomobject]@{DisplayName='Other application';WindowsInstaller=1} }
            { Assert-SunshineInstallerCompatibility } | Should -Not -Throw
        }
    }
}

Describe 'Install failure boundaries and readback' {
    InModuleScope SunshineInstall {
        BeforeEach {
            $script:testRoot=Join-Path $TestDrive 'Installed Sunshine'
            $script:testInstaller=Join-Path $TestDrive 'Sunshine.exe'
            $null=New-Item -ItemType Directory -Path $script:testRoot -Force
            Set-Content -LiteralPath (Join-Path $script:testRoot 'sunshine.exe') -Value 'installed fixture'
            Set-Content -LiteralPath $script:testInstaller -Value 'installer fixture'
            Mock Read-SunshineInstallerManifest { [pscustomobject]@{source_commit=('a' * 40)} }
            Mock Assert-SunshineInstallerCompatibility {}
            Mock Get-SunshineInstallRoot { $script:testRoot }
            Mock New-SunshineConfigurationBackup { $null }
            Mock Stop-SunshineForInstall {}
            Mock Start-Process { [pscustomobject]@{ExitCode=0} }
            Mock Get-Service { [pscustomobject]@{Status='Running'} }
            Mock Start-Service {}
            Mock Write-Information {}
        }

        It 'verifies the package before stopping or executing anything' {
            Mock Read-SunshineInstallerManifest { throw 'hash mismatch' }
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*hash*'
            Should -Invoke Stop-SunshineForInstall -Times 0 -Exactly
            Should -Invoke Start-Process -Times 0 -Exactly
        }
        It 'supports a dry run without backup, service interruption or installation' {
            Invoke-SunshinePackageInstall $script:testInstaller -WhatIf
            Should -Invoke New-SunshineConfigurationBackup -Times 0 -Exactly
            Should -Invoke Stop-SunshineForInstall -Times 0 -Exactly
            Should -Invoke Start-Process -Times 0 -Exactly
        }
        It 'refuses an incompatible install before backup or service interruption' {
            Mock Assert-SunshineInstallerCompatibility { throw 'MSI installation' }
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*MSI*'
            Should -Invoke New-SunshineConfigurationBackup -Times 0 -Exactly
            Should -Invoke Stop-SunshineForInstall -Times 0 -Exactly
        }
        It 'does not interrupt Sunshine if backup fails' {
            Mock New-SunshineConfigurationBackup { throw 'backup failed' }
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*backup*'
            Should -Invoke Stop-SunshineForInstall -Times 0 -Exactly
            Should -Invoke Start-Process -Times 0 -Exactly
        }
        It 'does not run the installer if Sunshine cannot be stopped' {
            Mock Stop-SunshineForInstall { throw 'service stop failed' }
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*stop*'
            Should -Invoke Start-Process -Times 0 -Exactly
        }
        It 'waits for a silent NSIS installer with the existing install directory last' {
            Invoke-SunshinePackageInstall $script:testInstaller
            Should -Invoke Start-Process -Times 1 -Exactly -ParameterFilter {
                $FilePath -eq $script:testInstaller -and $ArgumentList -eq "/S /D=$script:testRoot" -and
                $Wait -and $PassThru -and $WindowStyle -eq 'Hidden'
            }
        }
        It 'stops after an installer failure without retrying' {
            Mock Start-Process { [pscustomobject]@{ExitCode=7} }
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*exit code 7*'
            Should -Invoke Start-Process -Times 1 -Exactly
            Should -Invoke Get-Service -Times 0 -Exactly
        }
        It 'does not claim success when the executable is absent' {
            Remove-Item -LiteralPath (Join-Path $script:testRoot 'sunshine.exe')
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*expected Sunshine executable*'
        }
        It 'does not claim success when old configuration disappeared' {
            $script:testBackup=Join-Path $TestDrive 'backup'
            $null=New-Item -ItemType Directory -Path (Join-Path $script:testBackup 'config') -Force
            Set-Content -LiteralPath (Join-Path $script:testBackup 'config\sunshine.conf') -Value 'old configuration'
            Mock New-SunshineConfigurationBackup { $script:testBackup }
            { Invoke-SunshinePackageInstall $script:testInstaller } | Should -Throw '*configuration file is missing*'
        }
    }
}

Describe 'Configuration backup' {
    InModuleScope SunshineInstall {
        It 'copies credentials and settings without modifying the source and restricts the backup ACL' {
            $root=Join-Path $TestDrive 'source'
            $null=New-Item -ItemType Directory -Path (Join-Path $root 'config\credentials') -Force
            Set-Content -LiteralPath (Join-Path $root 'config\sunshine_state.json') -Value '{"fixture":"pairing"}'
            Set-Content -LiteralPath (Join-Path $root 'config\credentials\key.pem') -Value 'synthetic credential'
            $previous=$env:LOCALAPPDATA
            try {
                $env:LOCALAPPDATA=$TestDrive
                $backup=New-SunshineConfigurationBackup $root
                (Get-FileHash -LiteralPath (Join-Path $backup 'config\sunshine_state.json')).Hash |
                    Should -Be (Get-FileHash -LiteralPath (Join-Path $root 'config\sunshine_state.json')).Hash
                Test-Path -LiteralPath (Join-Path $backup 'config\credentials\key.pem') | Should -BeTrue
                $acl=Get-Acl -LiteralPath $backup
                $acl.AreAccessRulesProtected | Should -BeTrue
                @($acl.Access).Count | Should -Be 2
                @($acl.Access | Where-Object {$_.IsInherited}).Count | Should -Be 0
            } finally { $env:LOCALAPPDATA=$previous }
        }
        It 'creates no backup for a first installation' {
            New-SunshineConfigurationBackup (Join-Path $TestDrive 'missing') | Should -BeNullOrEmpty
        }
    }
}

Describe 'Sunshine shutdown for replacement' {
    InModuleScope SunshineInstall {
        BeforeEach {
            Mock Stop-Service {} -RemoveParameterType InputObject
            Mock Stop-Process {}
            Mock Get-Service { $null }
            Mock Get-Process { $null }
        }
        It 'waits for services and targets only Sunshine desktop processes' {
            Mock Get-Service {
                if ($Name -eq 'SunshineService') {
                    $service=[pscustomobject]@{Status='Running'}
                    $service | Add-Member ScriptMethod WaitForStatus {param($state,$timeout)}
                    return $service
                }
            }
            Mock Get-Process {
                $process=[pscustomobject]@{Id=42}
                $process | Add-Member ScriptMethod WaitForExit {param($timeout) return $true}
                return $process
            } -ParameterFilter { $Name.Count -eq 2 -and $Name -contains 'sunshine' -and $Name -contains 'sunshinesvc' }
            Stop-SunshineForInstall
            Should -Invoke Stop-Service -Times 1 -Exactly
            Should -Invoke Stop-Process -Times 1 -Exactly -ParameterFilter { $Id -eq 42 -and $Force }
        }
        It 'refuses to proceed if a desktop process remains alive' {
            Mock Get-Process {
                $process=[pscustomobject]@{Id=42}
                $process | Add-Member ScriptMethod WaitForExit {param($timeout) return $false}
                return $process
            }
            { Stop-SunshineForInstall } | Should -Throw '*did not exit*'
        }
        It 'does not kill desktop processes after a service stop failure' {
            Mock Get-Service { [pscustomobject]@{Status='Running'} }
            Mock Stop-Service { throw 'service unavailable' } -RemoveParameterType InputObject
            { Stop-SunshineForInstall } | Should -Throw '*service unavailable*'
            Should -Invoke Stop-Process -Times 0 -Exactly
        }
    }
}
