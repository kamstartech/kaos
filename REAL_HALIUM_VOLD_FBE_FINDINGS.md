# Real Halium: Does the LXC Guest Need vold / FBE Decryption?

> Investigation of the reference Halium source tree at `/home/jimmy/halium`
> (full AOSP checkout, SP2A / **Android 12.1.0_r22**, LineageOS 19.1 base).
> Date: 2026-07-31. Author: agent investigation.
> Purpose: reliable record of how real Halium treats vold / FBE / /data
> decryption inside its LXC guest container, so the Kaos project can decide
> whether to keep chasing qseecomd or stop caring (matching the surfaceflinger
> precedent).

---

## 1. How to read this tree (scope caveat — read this first)

- **`lxc-android-config` itself is NOT checked out** in `/home/jimmy/halium`.
  The manifest
  `/home/jimmy/halium/halium/devices/manifests/halium_halium_arm64.xml:3`
  references `device/halium/halium_arm64`
  (`Halium/android_device_halium_halium_arm64`, revision `halium-12.0`), but
  that project is absent from the checkout. The base AOSP manifest
  `/home/jimmy/halium/android/default.xml:16-17` is plain LineageOS 19.1
  (`default revision="refs/heads/lineage-19.1"`) with **no hybris repos**.
  So the *config-level* scripts (`pre-start.sh`, `mount.sh`,
  `mount-halium-overlay`, `service_disable.rc`) cannot be cited from this
  tree.
- **What IS present is the authoritative Halium patch set**:
  `/home/jimmy/halium/hybris-patches/` (67 patch files, applied at build time
  by `/home/jimmy/halium/hybris-patches/apply-patches.sh`). These patches
  show exactly how real Halium rewrites AOSP `vold`, `init`, and framework
  source. **This is even stronger evidence than the config scripts**, because
  it is the source-level truth of what the guest runs.
- The checked-out AOSP source under `system/vold`, `system/core`, etc. is the
  **pristine upstream** (patches are *not* pre-applied — verified: no "Halium"
  markers in `system/vold/MetadataCrypt.cpp` / `system/vold/main.cpp` /
  `system/core/init/builtins.cpp`). Citations below therefore distinguish:
  - `hybris-patches/...` = the Halium modification (what to apply), and
  - `system/...` = the pristine source it modifies (the surrounding flow).

---

## 2. Question restated

Does real, working Halium depend on `/data` FBE decryption succeeding inside
its LXC guest container? Or does it mask/disable vold (or otherwise not depend
on it) the same way it masks surfaceflinger for display — implying our project
should also stop chasing the qseecomd decrypt loop?

---

## 3. Direct answer

**Real Halium does NOT need guest-side FBE decryption. It actively disables the
entire FBE/decrypt path at three independent layers, and it forces the guest
framework to believe `/data` is unencrypted.** All real mounts — including
`/data` — are expected to be performed **host-side, outside the container**.
The guest's vold is kept running but neutered; its encryption/decryption code
is dead.

This is the same *philosophy* as the surfaceflinger handling (the guest must
not depend on a host-owned service), though implemented differently (see §9).

**Bottom line for qseecomd: stop chasing it for `/data` decryption.** Real
Halium never runs the decrypt path in the guest, so there is nothing for
qseecomd to support in that role. Mirror Halium's pattern instead
(see §10).

---

## 4. Evidence A — vold is patched to be non-fatal and encryption-disabled

### A1. vold must not exit when it cannot see logical partitions

`/home/jimmy/halium/hybris-patches/system/vold/0001-halium-do-not-exit-if-unable-to-find-logical-partiti.patch`

- Commit message (line 6): *"This is non-fatal as mounts can be handled
  outside LXC container."*
- In `main.cpp` → `process_config()`, the fatal failure when a logical
  partition cannot be resolved is downgraded to non-fatal and skipped
  (patch lines 22-25):
  ```c
  //PLOG(FATAL) << "could not find logical partition " << entry.blk_device;
  // Halium: make it non-fatal
  PLOG(ERROR) << "could not find logical partition " << entry.blk_device;
  continue;
  ```

### A2. FBE / metadata-encryption decryption is disabled at the source

`/home/jimmy/halium/hybris-patches/system/vold/0002-halium-do-not-attempt-to-mount-partitions-with-metad.patch`

- In `MetadataCrypt.cpp` → `fscrypt_mount_metadata_encrypted()`, the whole
  function body is replaced with a hard `false` (patch lines 24-26):
  ```cpp
  // Disabled for Halium
  return false;
  ```
  This is the exact function Android uses to mount a metadata-encrypted /
  FBE userdata partition in userspace. Real Halium returns `false`
  unconditionally — it never attempts decrypt.
- The same patch also kills the keymaster gate (see §6).

**Together A1+A2:** vold stays alive (`start vold` is still in the guest
init.rc — §5B) but its storage-encryption functionality is non-existent and it
tolerates not owning the partitions. vold is "along for the ride".

---

## 5. Evidence B — the guest init is forced to report `/data` as unencrypted

### B1. `mount_all` is faked inside the container

`/home/jimmy/halium/hybris-patches/system/core/0002-halium-init-modify-mount_all-to-skip-mounts-and-trig.patch`

- In `init/builtins.cpp` → `do_mount_all()`, the real
  `fs_mgr_mount_all()` call is commented out and replaced with a hardcoded
  result (patch lines 29-30):
  ```cpp
  // Halium: filesystem mount is handled outside container, always non-encrypted
  MountAllResult mount_fstab_result = {FS_MGR_MNTALL_DEV_NOT_ENCRYPTED, true};
  ```
- The same patch adds a fake `mount_all` to `rootdir/init.rc`
  (patch lines 43-45):
  ```
  on fs
      # Do fake mount_all for Halium
      mount_all /dev/null
  ```

### B2. The pristine flow this feeds — what `FS_MGR_MNTALL_DEV_NOT_ENCRYPTED` does

In the pristine source at `/home/jimmy/halium/system/core/init/builtins.cpp`:

- `do_mount_all()` (line 698) calls `fs_mgr_mount_all(&fstab, ...)` — the call
  Halium comments out.
- The result is processed by `queue_fs_event()`; the `FS_MGR_MNTALL_DEV_NOT_ENCRYPTED`
  case (lines 608-610) does:
  ```cpp
  } else if (code == FS_MGR_MNTALL_DEV_NOT_ENCRYPTED) {
      SetProperty("ro.crypto.state", "unencrypted");
      ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
  ```

**Net effect:** inside a Halium guest, `ro.crypto.state` is *always*
`unencrypted` and the `nonencrypted` event always fires. The framework can
never observe an encrypted `/data` and therefore never expects vold (or
anything else) to decrypt it. Combined with A2, the decrypt path is dead on
both ends: vold refuses to do it AND init declares it unnecessary.

### B3. vold is still started — but only as a normal binder service

Pristine `/home/jimmy/halium/system/core/rootdir/init.rc`:
- `on early-fs` block, line 560-562 → `start vold`
- another `start vold` at line 1302

So vold.rc is **NOT masked/disabled** the way the surfaceflinger *service* is.
vold runs. What Halium disables is what vold would *do* (encrypt/decrypt).

---

## 6. Evidence C — the keymaster gate is killed

The same vold patch (A2) renames the wait-for-keymaster service out of
existence. `wait_for_keymaster.rc` in the pristine tree
(`/home/jimmy/halium/system/vold/wait_for_keymaster.rc:1`):

```
service wait_for_keymaster /system/bin/wait_for_keymaster
```

Halium rewrites the service line (patch 0002, line 37):

```
service wait_for_keymaster /system/bin/wait_for_keymaster_DISABLED
```

The binary `/system/bin/wait_for_keymaster_DISABLED` does not exist, so the
service can never start. In normal Android, `wait_for_keymaster` blocks vold
until keymaster is available before attempting metadata decryption. Halium
removes that dependency entirely.

---

## 7. Evidence D — other host-owned mounts are removed from the guest

### D1. First-stage mounts

`/home/jimmy/halium/hybris-patches/system/core/0003-halium-init-adjust-first-stage-mounts-for-Halium.patch`
- `/mnt` tmpfs mount restricted to recovery mode (line 25): *"Disabled for
  Halium, mounted by LXC config"*.
- `/apex` tmpfs mount similarly restricted (lines 59-63).

### D2. configfs (USB) — explicitly host-owned

`/home/jimmy/halium/hybris-patches/system/core/0016-halium-init.rc-also-disable-mounting-config-in-conta.patch`
- Commit message (lines 4-9): *"We've decided that we will handle everything
  USB in the host side. Leaving this mounted inside the container opens a
  chance for vendor HALs to mess with the USB configuration..."*
- `mount configfs none /config ...` commented out (lines 41-44).

This is the clearest statement in the tree of the **host-does-it-all**
philosophy: anything the host can own, the guest is stripped of.

### D3. odsign (on-device signing) disabled and never waited on

`/home/jimmy/halium/hybris-patches/system/core/0018-halium-init.rc-disable-and-do-not-wait-for-odsign-da.patch`
- `start odsign` commented out (line 21).
- All three `wait_for_prop odsign.*.done` waits commented out (lines 27, 37, 47, 57).
- odsign signs ART artifacts against keys derived from the /data-backed
  keystore; with `/data` treated as host-owned and unencrypted, the signing
  chain is simply not needed in the guest.

---

## 8. Evidence E — the surfaceflinger precedent (the comparison the question asked for)

`/home/jimmy/halium/hybris-patches/frameworks/native/0011-halium-libgui-do-not-attempt-to-get-SurfaceFlinger-s.patch`

- `Surface::composerService()` (lines 19-24) returns `nullptr`:
  ```cpp
  // Halium: SurfaceFlinger is not running
  return nullptr;
  ```
- `ComposerService::connectLocked()` (lines 104-111) no longer waits for the
  `"SurfaceFlinger"` service — the `waitForService<ISurfaceComposer>(name)`
  call is commented out, so `mComposerService` stays null and returns `false`.
- Every `Surface` method that would dereference the composer now guards with
  `if (composerService() == nullptr) return ...;` (lines 33-34, 45-47, 57-59,
  69-71, 81-83, 91-94).

**How SF is handled:** SF is stubbed at the *library* level — the guest's
libgui simply assumes SurfaceFlinger does not exist and never blocks on it.

**How vold is handled (contrast):** vold is kept as a running binder service
but its *functionality* is removed at the *source* level (A2) and declared
unnecessary via `ro.crypto.state=unencrypted` (B). Both implement the same
rule — *the guest must not depend on a host-owned service* — but SF is
removed wholesale while vold is retained-as-decoy.

---

## 9. Evidence F — related guest-service trimming (context)

- Media services disabled by binary rename (service points at a nonexistent
  `_HYBRIS_DISABLED` binary):
  `/home/jimmy/halium/hybris-patches/frameworks/av/0002-halium-disable-media-related-services.patch`
  — `cameraserver_HYBRIS_DISABLED` (line 21), `audioserver_HYBRIS_DISABLED`
  (line 31), `mediaserver_HYBRIS_DISABLED` (line 44). Commit message: *"Their
  functionality is provided by minimedia in droidmedia repo instead."*
- servicemanager no longer restarts the dead services:
  `/home/jimmy/halium/hybris-patches/frameworks/native/0001-hybris-Use-mini-services-and-disable-starting-unneed.patch`
  — `onrestart restart zygote/audioserver/media/surfaceflinger/.../keystore/gatekeeperd`
  all commented out (lines 23-32), replaced with minimal `minimedia`/`minisf`/
  `miniaf` services.

This confirms the general Halium pattern: **keep a minimal set of guest
services; anything the host provides is disabled, renamed away, or stubbed.**

---

## 10. Evidence G — keystore2 / keymint: not explicitly handled

Searched the entire `hybris-patches/` set for `keymint|keystore|keymaster`.
No patch disables keystore2 or the keymint HAL. The only keymaster-related
touches are:

- the `wait_for_keymaster_DISABLED` rename (Evidence C, in the vold patch),
- removal of `keystore` from servicemanager's restart chain (Evidence F).

**Implication:** keystore2 runs in the guest but with no keymint HAL and no
`wait_for_keymaster` gate. It fails/limps silently. **Nothing in the container
depends on it** — there are no Android apps needing app-scoped keys, and the
framework's odsign/signing chain is disabled (D3).

---

## 11. What the container's vold is actually FOR in real Halium

The tree's own comments answer this:

1. *"This is non-fatal as mounts can be handled outside LXC container."* —
   vold 0001, line 6. **vold does not own the mounts.**
2. *"Halium: filesystem mount is handled outside container, always
   non-encrypted"* — init 0002, line 29. **Mounting is the host's job; the
   guest is permanently non-encrypted.**
3. *"We've decided that we will handle everything USB in the host side."* —
   init 0016, lines 4-9. **Host-ownership is a deliberate design decision.**

So the container's vold is **not load-bearing for anything Halium itself
needs**. It is retained only because stock `init.rc` starts it and some binder
clients expect a vold service object to exist; its storage-encryption role is
replaced by host-side mounting of an unencrypted `/data`. It is literally
"along for the ride" as part of the generic init.rc.

---

## 12. Bottom-line recommendation for the Kaos project (qseecomd)

**Do not keep chasing qseecomd for `/data` FBE decryption.** Real Halium
provides a complete, proven pattern to stop needing it:

1. **Kill the decrypt path in vold** — make `fscrypt_mount_metadata_encrypted()`
   (the metadata/FBE decrypt entry point) return `false`, exactly like
   `hybris-patches/system/vold/0002` does.
2. **Force `ro.crypto.state=unencrypted`** in the guest's `mount_all` handling
   (fake the result as `FS_MGR_MNTALL_DEV_NOT_ENCRYPTED`), so nothing in the
   framework waits on decryption — exactly like `hybris-patches/system/core/0002`.
3. **Disable `wait_for_keymaster`** (rename the binary / comment the service),
   so vold never blocks on keymaster availability — exactly like vold 0002 does.
4. **Optionally** disable `odsign` + its `wait_for_prop` gates
   (init 0018) to remove the /data-keyed signing dependency from boot.

This mirrors the surfaceflinger precedent: decide the guest doesn't own
`/data` decryption, make that true at the source/init level, and stop
depending on the secure-world component (qseecomd) that would otherwise be
required to support it.

> Keep qseecomd **only if** another guest feature (camera, Wi-Fi secure-world,
> DRM) genuinely needs it. Do not run it for FBE.

---

## 13. Citation index (all under `/home/jimmy/halium/`)

| Topic | File | Lines |
|-------|------|-------|
| halium device manifest (not checked out) | `halium/devices/manifests/halium_halium_arm64.xml` | 3 |
| base AOSP manifest (no hybris repos) | `android/default.xml` | 16-17 |
| patch application script | `hybris-patches/apply-patches.sh` | 1-24 |
| vold: non-fatal missing partitions | `hybris-patches/system/vold/0001-halium-do-not-exit-if-unable-to-find-logical-partiti.patch` | 6, 22-25 |
| vold: FBE/metadata decrypt disabled | `hybris-patches/system/vold/0002-halium-do-not-attempt-to-mount-partitions-with-metad.patch` | 24-26 |
| vold: wait_for_keymaster renamed dead | `hybris-patches/system/vold/0002-...-with-metad.patch` | 37 |
| init: fake mount_all / forced unencrypted | `hybris-patches/system/core/0002-halium-init-modify-mount_all-to-skip-mounts-and-trig.patch` | 29-30, 43-45 |
| pristine mount_all call | `system/core/init/builtins.cpp` | 698 |
| pristine ro.crypto.state=unencrypted | `system/core/init/builtins.cpp` | 608-610 |
| pristine `start vold` (early-fs) | `system/core/rootdir/init.rc` | 560-562 |
| pristine `start vold` | `system/core/rootdir/init.rc` | 1302 |
| init: /mnt + /apex host-owned | `hybris-patches/system/core/0003-halium-init-adjust-first-stage-mounts-for-Halium.patch` | 25, 59-63 |
| init: configfs disabled (host-owned USB) | `hybris-patches/system/core/0016-halium-init.rc-also-disable-mounting-config-in-conta.patch` | 4-9, 41-44 |
| init: odsign disabled | `hybris-patches/system/core/0018-halium-init.rc-disable-and-do-not-wait-for-odsign-da.patch` | 21, 27, 37, 47, 57 |
| surfaceflinger stubbed in libgui | `hybris-patches/frameworks/native/0011-halium-libgui-do-not-attempt-to-get-SurfaceFlinger-s.patch` | 20-24, 33-34, 45-47, 57-59, 69-71, 81-83, 91-94, 104-111 |
| media services renamed dead | `hybris-patches/frameworks/av/0002-halium-disable-media-related-services.patch` | 21, 31, 44 |
| servicemanager restart chain trimmed | `hybris-patches/frameworks/native/0001-hybris-Use-mini-services-and-disable-starting-unneed.patch` | 23-32 |
| pristine wait_for_keymaster.rc | `system/vold/wait_for_keymaster.rc` | 1 |

---

## 14. Caveats / things not verifiable from this tree

1. **The lxc-android-config layer is absent** (see §1). The config-level
   `service_disable.rc` masking of surfaceflinger — the specific mechanism the
   question referenced — lives in that repo, not here. The source-level
   evidence in this document is independent and conclusive, but if you want the
   verbatim `.rc` masking you must pull `Halium/lxc-android-config` (or
   inspect an installed Halium rootfs overlay).
2. **This tree is Android 12.1 (SP2A); our device tree is Android 15.**
   Treat as "how real Halium approaches this," not API-identical. The vold
   code paths (`MetadataCrypt.cpp`, `wait_for_keymaster.rc`) differ somewhat on
   Android 15 (e.g. keymint v2, `storaged` changes), but the *strategy* —
   disable decrypt, force unencrypted, kill keymaster gate — is version-stable.
3. **Patches are the reference, not the applied tree.** If a future checkout
   applies them via `apply-patches.sh`, verify the hunks still apply to the
   vendored source before relying on them.
4. No documentation/comment in this tree beyond the patch messages quoted
   above explains vold's guest role — the patch messages ARE the documentation.
