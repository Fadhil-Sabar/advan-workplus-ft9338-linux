# FT9201/FT9338 fingerprint support for Linux

[Bahasa Indonesia](README.id.md)

Linux support for the FocalTech USB fingerprint reader identified as
`2808:9338`. The driver integrates with
[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) and `fprintd`,
so the reader can be used by desktop settings, PAM, and command-line tools.

## Status

Enrollment and verification work end-to-end on the tested hardware.

> **Hardware disclaimer:** this driver was developed and validated specifically
> with the built-in fingerprint reader in an **Advan Workplus Ryzen 5 6600H**
> laptop. Compatibility is determined by the USB ID and sensor revision, not by
> the FT9201 product name alone. Success on other Advan Workplus revisions or
> other laptops is not guaranteed.

| Item | Tested value |
| --- | --- |
| Tested computer | Advan Workplus Ryzen 5 6600H |
| USB ID | `2808:9338` |
| Sensor chip | `0x6893` / FT9338W family |
| Image | 64 x 80 pixels, 500 DPI |
| Host architecture | x86-64 |
| Tested distribution | Fedora |
| Integration | libfprint, fprintd, KDE/GNOME, PAM |

The final verification result on the test system was:

```text
Verify result: verify-match (done)
```

This repository targets the device above. Other FT9201 product IDs or sensor
revisions may use a different protocol and are not automatically supported.

### Distribution support

Only Fedora x86-64 has been verified on real hardware so far. The build and
installation flow should also apply to other conventional x86-64 distributions,
but those combinations are currently unverified.

| Distribution | Current status |
| --- | --- |
| Fedora/Nobara | Fedora tested; Nobara expected to use the same flow |
| Debian/Ubuntu | Expected to work; dependency names are documented below, but hardware testing is still needed |
| Arch/Manjaro | Expected to work after installing equivalent packages; unverified |
| openSUSE | Expected to work after installing equivalent `-devel` packages; unverified |
| NixOS, Alpine, immutable systems, or non-systemd systems | The current installer requires adaptation |
| ARM/AArch64 | Unsupported because the vendor matcher is an x86-64 DLL |

The current installer assumes systemd, udev, `/etc/udev/rules.d`,
`/usr/lib/libfprint-2`, and `/usr/local/lib/ft9201`. The driver source is less
distribution-specific than the installer, so alternative layouts can be
supported by adjusting `scripts/install.sh` or packaging the files natively.

## Quick installation

The repository includes a side-by-side installer. It leaves the distribution's
libfprint files untouched.

### 1. Install build dependencies

Fedora:

```bash
sudo dnf install git meson ninja-build gcc curl cabextract python3 \
  glib2-devel gusb-devel nss-devel pixman-devel libgudev-devel systemd-devel
```

Debian/Ubuntu equivalents:

```bash
sudo apt install git meson ninja-build gcc curl cabextract python3 \
  libglib2.0-dev libgusb-dev libnss3-dev libpixman-1-dev \
  libgudev-1.0-dev libsystemd-dev
```

### 2. Fetch the required vendor files

From this repository checkout:

```bash
./scripts/fetch-blobs.sh
```

The script downloads the FocalTech matcher from its signed Windows driver and
generates the firmware header currently required by the source tree. No
proprietary binary is committed to this repository.

### 3. Build

```bash
./scripts/build.sh
```

This downloads the tested libfprint release, registers the FT9201 driver, and
builds the library plus its enrollment and verification examples.

### 4. Install for fprintd

```bash
sudo ./scripts/install.sh
```

The installer:

- installs the custom libfprint under `/usr/local/lib/ft9201`;
- installs the vendor matcher and its W^X-safe prepared image;
- installs the udev rule for the reader;
- adds an `fprintd` systemd drop-in pointing only that service at the custom
  library; and
- reloads udev and restarts `fprintd`.

The distribution-owned libfprint remains unchanged.

### 5. Enroll a finger

```bash
fprintd-enroll
```

Enrollment can require up to 18 accepted samples. Continue until the command
reports `enroll-completed`; partial `enroll-stage-passed` output is not yet a
saved fingerprint.

For reliable samples:

- touch, then fully remove the finger before the next sample;
- vary the position slightly to cover the center and edges;
- keep the sensor and finger clean and dry; and
- do not repeatedly press the exact same area.

### 6. Verify

```bash
fprintd-verify
```

A successful setup ends with:

```text
Verify result: verify-match (done)
```

You can then enable fingerprint authentication in KDE or GNOME user settings.

## Test without installing

After building, the libfprint examples can be run directly:

```bash
sudo env \
  FT9201_ENGINE_DLL="$PWD/blobs/ftWbioEngineAdapter.dll" \
  LD_LIBRARY_PATH="$PWD/libfprint/build/libfprint" \
  "$PWD/libfprint/build/examples/enroll"
```

Then verify with:

```bash
sudo env \
  FT9201_ENGINE_DLL="$PWD/blobs/ftWbioEngineAdapter.dll" \
  LD_LIBRARY_PATH="$PWD/libfprint/build/libfprint" \
  "$PWD/libfprint/build/examples/verify"
```

Templates created by a root-run example belong to root and are separate from
the normal user's fprintd enrollment. Use `fprintd-enroll` for desktop and PAM
integration.

## How it works

The working `2808:9338` path performs these operations:

1. Initialize the USB bridge and identify the FT9338W-family sensor.
2. Poll the sensor until a finger is detected.
3. Capture one 5120-byte frame, representing a 64 x 80 grayscale image.
4. Pass the frame to FocalTech's `ftWbioEngineAdapter.dll` through the native
   in-process PE/WinBio compatibility layer.
5. Return enrollment or match results through the normal libfprint API.

No Wine process or Windows installation is required. The loader prepares a
file-backed image whose code pages are read/execute and whose writable data is
private read/write. It therefore remains compatible with fprintd's
`MemoryDenyWriteExecute` hardening and SELinux; security hardening does not need
to be disabled.

The active `2808:9338` capture path does not upload the legacy MCU firmware.
The generated firmware header remains a build dependency because the source
tree still contains the earlier bring-up path.

For the investigation history and protocol details, see
[FT9201-9338-WRAP-UP.md](FT9201-9338-WRAP-UP.md). That document is currently in
Indonesian. General notes about adapting the method to another device are in
[PORTING.md](PORTING.md).

## Troubleshooting

### Repeated `enroll-remove-and-retry`

Remove the finger completely between samples. If it continues, clean the sensor
with a dry microfiber cloth and dry the finger. In testing, oil on the sensor
caused long retry loops even though image capture itself was working.

### `NoEnrolledPrints` during verification

Enrollment did not reach its final commit, or it was performed as another user.
Run `fprintd-enroll` again as the same user and wait for `enroll-completed`.

### `enroll-unknown-error` or fprintd disconnects

Inspect the service log:

```bash
sudo journalctl -u fprintd --since "5 minutes ago" --no-pager
```

Reinstall the current build and restart the daemon:

```bash
sudo ./scripts/install.sh
sudo systemctl restart fprintd
```

### SELinux reports a write to `/memfd:ftengine (deleted)`

That message is associated with the older in-memory loader. Rebuild and rerun
the installer so that `ftWbioEngineAdapter.dll.image` is generated and
installed. The current loader uses that W^X-safe file-backed image.

## Uninstall

```bash
sudo ./scripts/install.sh --uninstall
```

This removes the side-by-side library, matcher files, udev rule, and systemd
drop-in. It does not need to restore the distribution's libfprint because that
library was never replaced.

## Proprietary components

The repository does not redistribute FocalTech binaries. The fetch script
retrieves:

| File | Purpose | Source |
| --- | --- | --- |
| `ftWbioEngineAdapter.dll` | Enrollment and matching engine | FocalTech signed Windows driver from Microsoft Update Catalog |
| `src/ft9201_fw.h` | Legacy build-time firmware header | Extracted from the community-hosted FocalTech Linux package |

The matcher is proprietary and x86-64-only. The open driver and compatibility
loader cannot correct defects inside the vendor algorithm.

### Windows driver reference

The matcher used by this project comes from FocalTech's signed Windows biometric
driver. `scripts/fetch-blobs.sh` downloads the package directly from Microsoft's
Windows Update CDN:

- [Microsoft Update Catalog search: FocalTech Electronics Biometric](https://www.catalog.update.microsoft.com/Search.aspx?q=FocalTech%20Electronics%20Biometric)
- [Exact Microsoft CAB used by the fetch script](https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab)

The CAB is used only as the source of `ftWbioEngineAdapter.dll`. Installing that
Windows driver package on Linux is neither required nor supported.

## Credits

- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) for the Linux
  fingerprint framework.
- [banianitc/ft9201-fingerprint-driver](https://github.com/banianitc/ft9201-fingerprint-driver)
  for earlier FT9201 protocol research.
- [mrrbrilliant/ft9201-static](https://github.com/mrrbrilliant/ft9201-static)
  for the archived FocalTech Linux package used as a reference.
- [uunicorn/synaWudfBioUsb-sandbox](https://github.com/uunicorn/synaWudfBioUsb-sandbox)
  and related work for Windows biometric-driver tracing techniques.

## License

The driver, loader, and scripts in this repository are licensed under
LGPL-2.1-or-later, matching libfprint. FocalTech's DLL and firmware remain the
property of their respective owner and are not relicensed or distributed here.
