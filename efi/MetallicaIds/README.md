# MetallicaIds.efi

UEFI driver for OpenCore's `Drivers` folder. It reads the real SMBIOS system
product name and serial number from the firmware and saves them to the root of
the EFI System Partition as `Metallica-Ids.txt`.

Why: Metallica MIS / Prometheus sensors (`06cb:009a`, `138a:0097`, `138a:009d`)
check these two strings when a session is opened. A sensor that Windows or
Linux paired first needs the same two strings from HTID (`--host-product`,
`--host-serial`). On macOS they cannot be read, because OpenCore spoofs the
SMBIOS.

## Install

1. Copy `MetallicaIds.efi` to `EFI/OC/Drivers/` on your EFI partition.
2. In `config.plist`, under `UEFI > Drivers`, add an entry with `Path` set to
   `MetallicaIds.efi` and `Enabled` set to true.
3. Reboot once.
4. Mount the EFI partition in macOS. The file is at its root, next to the `EFI`
   folder: `Metallica-Ids.txt`.

## Output

```
product_name=<system product name>
product_serial=<system serial number>
sys_vendor=<manufacturer>
product_version=<system version>
looks_spoofed=no
```

Use `product_name` and `product_serial` as the `--host-product` and
`--host-serial` values. Trailing spaces are trimmed.

If it says `looks_spoofed=yes`, the table was already patched by OpenCore when
the driver ran, and those values are useless. A file holding real values
(`looks_spoofed=no`) is never overwritten by spoofed ones, and an unchanged
file is not rewritten.

## What it touches

Only `Metallica-Ids.txt` on the volume it was loaded from. No NVRAM, no
variables, no network. It always returns success, so it cannot block boot.

## Build

Linux with `gnu-efi`: `sudo apt install gnu-efi build-essential`, then `make`.
