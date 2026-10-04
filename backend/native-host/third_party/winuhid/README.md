# WinUHid, vendored

Upstream: https://github.com/cgutman/WinUHid, commit `d6cebbef5c7909168d1f881185be8f607d6aefd4`
(28/05/2025), MIT licence (`LICENSE`), by Cameron Gutman.

A UMDF2 driver that sits above Windows' in-box Virtual HID Framework (VHF) and lets a user-mode
process create HID devices from any report descriptor, then feed their input reports and answer
their output, get-feature and set-feature requests. MoonlightWeb's HID passthrough builds its
Windows driver, "MoonlightWeb Virtual HID", from it (plan P1; study
`docs/design/hid-passthrough-study.md`).

Kept from upstream, with its folder names so its relative includes still resolve:

- `WinUHid Driver/`: the driver (`WinUHid.c`, `WinUHid.h`, `Public.h`, `Trace.h`) and its INF;
- `WinUHid/`: the client library (`WinUHid.cpp`, `WinUHid.h`, `pch.*`).

Left out: the sample devices (`WinUHidDevs`), the unit tests, the WiX installer and its root CA
helper, the Visual Studio solution.

The build project, our INF and the signing script live in `backend/native-host/drivers/vhid/`.
MoonlightWeb's changes to these files are listed below, each one in its own commit.

## MoonlightWeb changes

None yet: this is upstream as of the commit above.
