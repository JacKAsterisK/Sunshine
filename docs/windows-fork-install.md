# Install this Sunshine fork on Windows

These entrypoints support x64 Windows. They build the current checkout with the project's
MSYS2 UCRT64 toolchain and package it with the existing NSIS installer. They do not fetch
an upstream Sunshine executable or switch source branches.

## Install a build on another machine

Build once on your build PC, then copy `cmake-build-windows/cpack_artifacts/Sunshine-fork-windows.zip`
to the Sunshine host. Extract the whole ZIP to a local folder and double-click `install.bat`.
Accept the Windows elevation prompt. No compiler, Git, MSYS2 or Node is needed on that host.
The bundle includes the installer and its build receipt; keep them together.

The same bundle command updates an existing NSIS Sunshine installation. Existing configuration,
credentials and pairing state remain in place. Before stopping Sunshine, the script copies those
files to an owner/SYSTEM-only folder under `%LOCALAPPDATA%/SunshineForkBackups`. Keep that backup
private: it includes authentication material. An install/update interrupts active streams.

The script verifies the installer SHA-256 against its local build receipt, waits for the installer,
and checks the resulting executable and service. The receipt records the source commit and whether
the source tree was dirty; it is an integrity check, not an independent publisher signature.
Do not mix bundles or rename the installer. Installer or post-install failures stop the command;
inspect the installation and the backup before trying again.
For detailed elevated errors, run `install.bat` from an administrator terminal.

This entrypoint uses NSIS and refuses existing MSI products. To replace an MSI installation, use the
matching MSI upgrade/uninstall path first and retain its configuration; these scripts do not
automatically remove MSI products or portable installations.

Finish first-run credentials and Moonlight pairing at `https://localhost:47990` on the host
(or your configured web UI port). Confirm capture on the intended local Windows desktop.
The installer uses the existing project's service/firewall hooks; restrict Windows firewall
access for your deployment separately. VLAN/VPN changes are outside these scripts.

## Build and install from source

Clone your fork with Git, enter its folder, then run:

```bat
setup.bat
build.bat
install.bat -InstallerPath cmake-build-windows\cpack_artifacts\Sunshine.exe
```

`setup.bat` installs missing MSYS2 and native Node.js through Windows Package Manager and installs
the UCRT64 dependencies used by `ci-windows.yml`. Close other MSYS2 shells before its full update.
If a core MSYS2 update closes the shell, rerun `setup.bat` to finish the update. See
[MSYS2's update instructions](https://www.msys2.org/docs/updating/).

`build.bat` initializes the recorded Git submodules, builds the current source, and produces the
installer plus deployment ZIP. Documentation/tests are disabled for this deployment build; use the
normal project test workflow for source validation. WiX packaging is disabled, so .NET is not
needed. This path does not install Sunshine or stop an existing stream.

From a source checkout, `install.bat` without an installer argument builds before installing.
From an extracted bundle, it uses the adjacent installer instead. Build prerequisites must be
prepared before building; installing a bundle does not require running `setup.bat`.

For later source updates:

```bat
update.bat
update.bat -NoPull
```

The default update requires a clean checkout and fast-forwards its configured current branch.
`-NoPull` builds local changes without pulling. Both finish the build before stopping Sunshine
and installing the new package. Existing application settings and pairings are retained.

Run any entrypoint with `-Help`. Set `SUNSHINE_NO_PAUSE=1` to suppress the final pause for automation.
Set `SUNSHINE_MSYS2_ROOT` for a non-default MSYS2 location. Set `SUNSHINE_BUILD_JOBS` to change the
default four parallel compiler jobs. Every batch file returns the underlying command's exit code.

## Recovery

Keep the prior installer bundle as well as the protected configuration backup. If an update fails,
inspect the service and installer logs before rerunning it; a reported failure can leave some files
updated. Stop Sunshine before manually restoring configuration from the backup to its original
location, and restore the original credentials directory permissions. Then install the known-good
package and verify the service, desktop capture and Moonlight pairing. The script deliberately does
not delete configuration or perform an uncertain automatic rollback.
