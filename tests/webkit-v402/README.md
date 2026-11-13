# Windows host regression model

Run from Windows PowerShell:

```powershell
python tests/webkit-v402/run-windows-host.py --source <patched-XNU-root> --output <results-directory-on-F>
```

The runner extracts the current staged production definitions of bindfs_webkit_fileid, bindfs_prepare_webkit_identity, bindfs_getattr, bindfs_webkit_native_lookup, bind_lookup, bindfs_webkit_identity_check, and bindfs_vget directly from source. The generated C includes original path/line directives. The extraction manifest records complete source SHA-256 and extracted-function SHA-256; no production implementation is copied into the test model.

The host model uses a small in-memory native and alias tree, exact vnode pointer identity, two different volume identifiers, mock VGET, caller-context checks, explicit retained iocounts, and injected lookup/getattr/VGET/allocation errors. Every scenario checks balanced references. Preparation additionally requires its lower root iocount at every subordinate lookup/getattr/VGET; this targets the previous released-root bug.

Covered cases include final component, simulated directory-relative lookup, full paths, dot components, case folding, NOFOLLOW, native canonical parent/name, all five native bundle reverse identities, alias reverse lookup via the extracted bindfs_vget including held roots and path/error cleanup, mount restrictions, permissions and cleanup, FSID consistency, and the extracted boot selfcheck. NOCROSS covered-view tests model the flags used by bulk fallback; the same request subsequently made without NOCROSS must return native identity.

Scope limitations: this does not compile or run XNU, APFS, real namei, MAC policy, namecache, mount lifecycle, actual file descriptors, getattrlistbulk/readdir, or LaunchServices. Case folding, authorization responses, absolute /System mount dispatch, reference operations, and lower VGET are host contracts. The absolute test explicitly maps /System to the simulated alias mount; it does not prove real absolute traversal. NOCACHE tests inspect the flag passed to the node constructor; they do not implement or prove kernel namecache behavior. The test cannot establish LS database registration or iSH launch success. Native '..' deliberately retains its canonical parent chain.

Outputs are under out/: generated C, extraction-manifest.json, compile.log, run.log, result.json, and the native Windows executable. The runner rejects non-Windows environments; no WSL is used.

The default run also compiles two generated-only negative controls: releasing the preparation root before subordinate lookup, and restoring a last-component gate. Both must fail the intended assertion. The production files remain unchanged. The alias-root inode 2 and Library parentid 2 are reverse-resolved through the extracted bindfs_vget, and alias/native IDs are tested in their own volume namespaces.

Preparation now records certified IDs with native mode disabled. Normal-path scenarios activate the view by calling the extracted transactional selfcheck; explicit tests verify that prepare alone leaves the covered alias view active and failed selfchecks disable native mode. Logging text is not validated by the model.


For a copy of these tests in a candidate repository, set both locations explicitly:

```powershell
python <test-directory>/run-windows-host.py --source <XNU-source-root> --output <host-results-directory>
# or
& <test-directory>/run-windows-host.ps1 -Source <XNU-source-root> -Output <host-results-directory>
```

The source root must contain bsd/miscfs/bindfs. The default source is ../source relative to the tests directory; the default output is ./out relative to the tests directory. A relative --source/--output is resolved from the invoking process's current directory. --generate-only / -GenerateOnly writes generated C and the extraction manifest without compiling or running.

Copy only these five reproducibility files: host-model.c, host-cases.c, run-windows-host.py, run-windows-host.ps1, README.md. Do not copy out/, generated C, binaries, or logs as test source. Final coverage includes a lowerroot marked VROOT: normal Library lookup remains an alias and must not be mistaken for an identity-query EIO.
