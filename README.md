# UniXNU iSH V4.0.2 build candidate

This candidate repairs the filesystem identity path used to discover the existing OS27 WebKit extensions. iSH installs and its main executable starts, but its WKWebView terminal has remained blank. The previous V4.0.1 workaround did not pass device acceptance.

This is a local, uncompiled candidate based on the working RAM8 build commit `545526b03857c35979360975afdc5fc527364d61`. It is not a flashable image or a claim that iSH now works. All stable source pins, touch/HID overlays, GPU code, 8 GiB memory, generic 215 GiB Data seed and boot packaging implementation are retained. The failed GPU V4.1/V4.2 performance changes are not included.

## Runtime change

A single appended patch changes five XNU files. The authenticated SystemOS bindfs view now enters the native APFS `Library/ExtensionKit` directory consistently for normal complete-path and directory-relative lookups. Explicit NOCROSS requests retain the covered bindfs view for directory enumeration; each view has consistent filesystem and file identities. The boot self-check alone activates native mode, and revokes it on failure. The board adds only `webkit-native-view=1` relative to the stable RAM8 board.

See [repair rationale and limitations](docs/ISH-V4.0.2-REPAIR.md) and [host evidence](evidence/v4.0.2-host-validation.json).

## Build entries

- Codemagic: branch `ram8-webkit-v4.0.2`, workflow `unixnu-ish-v402-handoff`, display name `UniXNU iSH V4.0.2 ARM64e boot handoff`.
- GitHub Actions: `.github/workflows/os27-ram8.yml`, manual dispatch only.
- Codemagic handoff: `os27-ram8-ish-v4.0.2-handoff.zip` plus SHA-256.

The Codemagic configuration retains the fixed ipsw ARM64 release and checksum used to resolve the earlier Homebrew cask failure. `ci/native_build.py` is the unchanged stable implementation, including its final readback checks. Cloud construction produces a generic provisional handoff, not a device-personalized boot image. Local personalization and the user's reboot timing remain separate steps. No cloud job has been started for this candidate.

## Windows regression

After the pinned author patches and this candidate patch have been applied to an XNU source tree:

```powershell
python tests/webkit-v402/run-windows-host.py --source <patched-XNU-root> --output <results-directory-on-F>
```

The host runner extracts seven production functions from that tree. It models vnode references, identity, lookup and error injection. It does not execute real APFS, XNU namei, LaunchServices, or iSH. See [test contracts](tests/webkit-v402/README.md). No WSL is needed.
