# iSH V4.0.2: repair the WebKit directory identity path

## Problem and evidence

The installed iSH application launches, but its WebKit terminal remains blank. Historical LaunchServices observations found the WebKit extension lookup/registration path failing. V4.0.1 attempted a native-directory view but still failed device acceptance; its captured boot snapshots stopped at stage 6/status 0, not a successful native stage 7. That pair did not establish that the workaround had activated. The retained run contained no attributable whole-system panic stack, so the reported later crash is not explained by that capture alone.

The source audit established concrete defects independent of whether each caused that crash: lookup of Library without the required start-vnode iocount, different identities for full paths and final-component/directory-relative access, and partial activation/failure state that could be mistaken for success. V4.0.2 fixes those mechanisms rather than inventing LaunchServices database rows.

## Baseline and scope

The baseline is build commit `545526b03857c35979360975afdc5fc527364d61`, Apple XNU `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` plus the exact 94 ordered stable author/overlay patches. The stable XNU tree is `24b742123ebdf8b88638ab6b8bba4facb128523a`. The new patch applies directly after those 94 patches; it does not stack on the V4 or V4.0.1 patches.

The five changed files are `bsd/kern/kern_exec.c` and `bsd/miscfs/bindfs/{bindfs.h,bind_subr.c,bind_vfsops.c,bind_vnops.c}`. Touch, HID, GPU, UEFI, verified block storage, RAM size, Data seed, root images and general mount topology are unchanged. The only board boot-argument addition is `webkit-native-view=1`.

## Implementation

1. **Reference ownership.** Preparation holds the start vnode through all subordinate lookups, file-attribute reads and reverse lookups. The new VGET path separately holds lower and alias roots while using them. Success, authorization failure, allocation failure and lookup failure release all acquired references.
2. **Object-based directory transition.** Preparation resolves the real Library and ExtensionKit IDs under the authenticated original SystemOS mount. bind_lookup first uses the actual lower lookup, including APFS name handling, then recognizes the certified parent/child pair. It does not inspect the remaining pathname or reject ISLASTCN, USEDVP, UNIQUE or NODOTDOT by default. A caller-context inner lookup preserves permission checking. The resolved native object's canonical parent/name must already be Library/ExtensionKit; native parent/name and firmlink state are never rewritten.
3. **Two intentional namespace views.** Normal complete-path and directory-relative calls enter native APFS, so ordinary FSID/file-ID lookup and reverse lookup agree. Explicit NOCROSSMOUNT stays in the covered bindfs namespace, because XNU generic getattrlistbulk uses that flag and skips entries on lookup failure. Covered nodes remain uncached in the namecache and keep both FSID forms in the alias namespace. Alias VGET uses the covered path. Public alias root inode 2 resolves to aliasroot, including reverse lookup of Library's advertised parent. The bindfs node hash can still reuse covered wrappers; it does not publish them into the native name lookup path.
4. **Mount restrictions.** The transition requires the lower mount to carry the alias's NOSUID, NOEXEC, NODEV and QUARANTINE restrictions. With the boot argument enabled, the initial SystemOS APFS mount adds NOSUID to both the syscall flags and the APFS mount payload. This affects set-ID execution policy across SystemOS, not only ExtensionKit. It does not disable normal executables, code-signing or entitlement checks. Constraints such as caller-root/beneath, local-device and immovable-device lookup are checked instead of silently bypassed.
5. **Transactional activation and real status.** Preparation records certified IDs but leaves native mode off. The boot initializer obtains the bind root before userspace/lsd, enables the mode only while checking full and split lookups, a peer Frameworks directory, and the five native WebKit bundle identities. Failure clears the mode and records the actual errno. An absent self-check leaves it disabled. Stage 7 requires both native identity validation and the Info.plist probe; it does not mean LaunchServices registration succeeded.

The five existing bundle names are WebContentExtension, WebContentEnhancedSecurityExtension, WebContentCaptivePortalExtension, GPUExtension and NetworkingExtension (all `.appex`). No WebKit framework, iSH application, entitlement or LaunchServices database is replaced by this patch. Existing author cache reconstruction remains responsible for registration.

## Validation completed

- Exact 94-patch baseline rebuilt from pinned inputs with no provenance gaps.
- Candidate patch applied and reversed; reversal restores the stable tree exactly.
- Generated five source blobs are byte-identical to the source used by the host tests.
- Windows clang compiled seven extracted production functions; 147 cases and 81,167 assertions passed.
- Two generated-only negative controls failed as intended: releasing the preparation root too early, and restoring the last-component gate. Production source was not changed by those controls.
- Both manual build configurations parse, and native_build.py has no code difference from the stable build.

The host model checks references, pointer identity, namespace identity, selected traversal flags, authentication responses and failures. It does not run the complete kernel, real APFS/namecache/readdir/getattrlistbulk, MAC policy or LaunchServices. Whole-XNU ARM64e compilation and the iSH terminal acceptance test remain outstanding.

## Build input downloads

The first Codemagic build stopped while fetching the Apple DriverKit directory
listing from the GitHub REST API. The build now reads a pinned index for
DriverKit and TrustCache headers and fetches their bytes from the recorded
Apple commits. The Adreno firmware fetch also uses the existing locked source
path directly. Git blob checks and firmware size and SHA-256 checks still apply.

The GitHub workflow runs the offline downloader tests before materializing
the overlay. A separate local comparison against all 26 real Apple headers
matched the original downloader output. The three Adreno firmware files also
matched their existing locks.

## Deliberate limits

This is a narrow canonical directory junction, not a new mountpoint implementation. It does not synthesize `v_mountedhere` or a directory mountpoint attribute. Native `..` returns its canonical native Library, not an invented alias parent. The actual LaunchServices interpretation of this view must still be checked. The extra NOSUID policy is explicit, and closed-source SystemOS programs have not been exhaustively qualified under it.

Future device acceptance should use one normally booted V4.0.2 candidate with existing Data preserved, then check whether iSH displays a prompt and runs `echo UniXNU-OK`. If it fails, the already-added preparation/check error and activation lines distinguish a disabled filesystem repair from a later registration/launch failure. Do not count stage 7 alone as application success. Do not combine the test with GPU, touch, partition or Data-reset changes.

## Primary source basis

- [Apple XNU vnode.h](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/bsd/sys/vnode.h): vnode_lookupat start-vnode ownership contract.
- [Apple XNU vfs_lookup.c](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/bsd/vfs/vfs_lookup.c): final-component, constrained traversal and canonical identity handling.
- [Apple XNU vfs_attrlist.c](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/bsd/vfs/vfs_attrlist.c): generic bulk enumeration's USEDVP/NOCROSSMOUNT lookup and entry skipping.
- [Apple XNU vfs_syscalls.c](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/bsd/vfs/vfs_syscalls.c): readdir conversion and mount restrictions.
- [UniXNU source and patches](https://unixnu.com/xnu/source-and-patches): pinned source and ordered patch build route.
