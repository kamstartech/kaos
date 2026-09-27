# Session Handoff — 2026-09-27 (Kali cold-boot VFS crash: anon mnt_namespace root-cause chase)

Subtitle: Root-caused and fixed three real, mainline-confirmed bugs in this kernel's new-mount-API
backport (`fs/namespace.c`) that were causing a fully reproducible `Kernel BUG at mntput_no_expire` /
`Kernel BUG at propagate_one` / `Kernel BUG at umount_tree` crash on every Kali cold boot, triggered by
systemd's `fsmount()`/`open_tree(OPEN_TREE_CLONE)`/`move_mount()` usage. Two of the three fixes are
committed and confirmed working in isolation (their specific symptoms are gone). **The core crash still
happens** — a 4th, still-unidentified bug remains. Diagnostic instrumentation is in place and partially
run; the next boot's log should narrow it down further. Read this before touching
`kernel/xiaomi/sdm845/fs/namespace.c` or `fs/mount.h`.

**Standing constraint, inherited from earlier sessions and still true: never use `dmesg`** to investigate
Kali boot failures — it's cleared/unreliable across reboots. Use the persistent, disk-backed logs instead
(see §5). This whole investigation was done entirely through those logs plus targeted `printk`
instrumentation, never a live shell on a booted (crashing) Kali.

---

## 1. The crash

Kali (via the Kaos hybrid distro's `switch_root` handover) boots into systemd, which very quickly
(during early service startup — sysusers, tmpfiles, journald, timesyncd, etc.) hits a hard kernel oops,
repeatedly (5-11 times per boot observed), eventually falling back / rebooting into Android. Always one
of:

```
Kernel BUG at mntput_no_expire+0x44/0x230
Kernel BUG at umount_tree+0x60/0x2ec
Kernel BUG at propagate_one+0x130/0x1dc   (seen in earlier sessions, same family)
```

Root cause (confirmed via direct mainline source comparison, v5.0/v5.2 `torvalds/linux`): this kernel's
"new mount API backport" in `fs/namespace.c` — a pre-existing, self-documented "minimal implementation
targeted at systemd's credential-setup path" covering `fsopen`/`fsconfig`/`fsmount`/`open_tree`/
`move_mount`/`mount_setattr` — never gave detached mounts (from `fsmount()` or
`open_tree(OPEN_TREE_CLONE)`) a real anonymous `mnt_namespace`. Every such mount had `mnt_ns == NULL` for
its entire detached lifetime, sometimes spanning multiple syscalls and forks. `mntput_no_expire()`,
`attach_recursive_mnt()`, and `propagate_one()` are all faithfully ported from mainline, and mainline's
versions of these functions assume `mnt_ns == NULL` is only ever a **brief, transitional** state (during
`clone_mnt()` itself) — never an ongoing one. This mismatch is what corrupts kernel state and crashes.

Real mainline's actual mechanism (`open_detached_copy()`, `fsmount()`) gives every detached mount tree a
real anon `mnt_namespace` immediately via `alloc_mnt_ns(user_ns, /*anon=*/true)`, and tears it down again
via `dissolve_on_fput()`, hooked into the generic `fs/file_table.c` `__fput()` path via a
`FMODE_NEED_UNMOUNT` file flag. This backport does **not** replicate the `FMODE_NEED_UNMOUNT`/`__fput()`
mechanism (a deliberate scope decision — it would mean touching universal, core VFS dispatch code instead
of staying inside this file's already-isolated new-mount-API section). Instead it uses the mount's own
already-isolated `mount_fops.release` callback to play both roles mainline splits across
`dissolve_on_fput()` + the generic `__fput()`'s own `path_put()`.

---

## 2. What's fixed and committed

Two commits on `kamstartech/lineage-22.2-nethunter` (kernel repo `kernel/xiaomi/sdm845`), **2 commits
ahead of the tracked remote, not yet pushed**:

- `34c83c3ed41b` — unrelated `drivers/tty/pty.c` build fix (missing `#include <linux/mount.h>`, found via
  a pasted compile log earlier this session; confirmed working, not part of the VFS investigation).
- `0c23db9f8180` — the core anon-`mnt_namespace` fix. Full commit message has all the detail; summary:
  - `alloc_mnt_ns()` gains a `bool anon` param. Anon → `seq = 0` (`is_anon_ns()`, `fs/mount.h:34`,
    `ns->seq == 0`), skips `ns_alloc_inum()` (never visible in `/proc`).
  - `kaos_attach_anon_ns()` (`fs/namespace.c:3723`) — called from `fsmount()` and `open_tree()`'s clone
    branch — gives a freshly detached mount tree a real anon namespace immediately.
  - `kaos_dissolve_detached_mnt()` (`fs/namespace.c:3779`) — called from `mount_fops_release()` — ported
    from mainline's `dissolve_on_fput()`: tears the tree down via `umount_tree(m, UMOUNT_CONNECTED)` if
    still anon (never attached), or a plain `mntput()` if `move_mount()` already reassigned it to a real
    namespace via `commit_tree()`.
  - `attach_recursive_mnt()` (`fs/namespace.c:2127`)'s `move_mount()` branch does mainline's
    `list_del_init(&source_mnt->mnt_ns->list)` before `commit_tree()` reassigns `mnt_ns`, plus an
    **added-beyond-mainline** explicit `free_mnt_ns(old_anon_ns)` afterward — mainline relies on its
    `FMODE_NEED_UNMOUNT` mechanism for that cleanup, which isn't replicated here, so it has to happen
    explicitly or the old anon namespace object leaks on every successful `move_mount()`.
  - **Bug found + fixed:** `clone_mnt()` (`fs/namespace.c:1048`) was missing mainline's trailing
    `else { CLEAR_MNT_SHARED(mnt); }` for the `CL_PRIVATE` case. A `CL_PRIVATE` clone (e.g. non-recursive
    `open_tree(OPEN_TREE_CLONE)`) kept `MNT_SHARED` copied wholesale from the source's `mnt_flags` while
    `mnt_group_id` was correctly zeroed and `mnt_share` left unlinked — an invariant violation.
    **Confirmed live** via the `kaos-mount-api` diagnostic log showing `shared=4096` on a clone that
    should've been private, immediately preceding an oops. **Confirmed fixed**: rebuild after this fix
    showed `shared=0` in the same scenario, and the `ida_remove` warning (next bullet) also disappeared —
    but the core crash did not go away (see §4).
  - **Bug found + fixed:** `free_mnt_ns()` (`fs/namespace.c:3003`) called `ns_free_inum()`
    unconditionally. Anon namespaces never call `ns_alloc_inum()` in the first place (see above), so this
    hit `ida_remove()` on an unallocated id — logged as `ida_remove called for id=... which is not
    allocated`, on essentially *every* `fsmount()`-then-close that's never attached (i.e. constantly
    during a normal systemd boot: sysusers, tmpfiles, etc.). Fixed with the same `is_anon_ns()` guard
    mainline's `free_mnt_ns()` already has. **Confirmed fixed**: the warning is completely gone in the
    build after this fix.
  - Also: a `move_mount()` `path_put(&p)` fix for the `lock_mount()` failure branch (a real path-reference
    leak in the user's own independently-written use-after-free fix for this function — reviewed this
    session, one bug found and fixed, does not itself affect the crash) and a forward declaration for
    `free_mnt_ns()` (needed because `attach_recursive_mnt()`, defined earlier in the file, now calls it
    directly — caught during self-review, real compile-breaker if missed).

---

## 3. What's NOT fixed yet — the actual current blocker

Even with both bugs above confirmed fixed (kernel build `#58`, commit `g0c23db9f8180`, then `#59` with an
uncommitted additional change — see §4), **the crash still happens**, now consistently as
`Kernel BUG at mntput_no_expire`, always at the exact same call site:

```
mntput_no_expire+0x44/0x230
kaos_dissolve_detached_mnt+0x140/0x174   <- the LAST call in the function: the "not anon" branch's mntput(mnt)
mount_fops_release+0x14/0x20
__fput -> ____fput -> task_work_run -> do_notify_resume   (i.e. a close(fd) on a detached-mount fd)
```

This means `is_anon_ns(ns)` is evaluating **false** for a mount that was *just* given a fresh anon
namespace by `kaos_attach_anon_ns()` moments earlier (confirmed via the `kaos-mount-api` log line printed
inside `open_tree()`/`fsmount()` right after attach succeeds — `mnt_ns=<non-null>` every time). Something
is invalidating that state before the fd closes.

**Smoking gun, from the diagnostic added this session** (`KAOS_MNTAPI_LOG` in `kaos_dissolve_detached_mnt`,
logs `mnt`, `m`, `ns`, `ns->seq`, and the anon-check result): in the broken cases, `ns->seq` — which
should always be either `0` (anon) or a small monotonic integer from `atomic64_add_return(1,
&mnt_ns_seq)` (real namespace) — instead decodes to genuine ARM64 kernel virtual addresses
(`0xffffffc9...`, matching this device's actual linear-map VA range), clustered in tight groups with an
**exact 448-byte stride** between consecutive readings. This is not random-looking corruption; it looks
like either (a) a dangling `mnt_ns` pointer whose backing memory has been freed and reused for something
else entirely (classic UAF — `struct mount`'s `mnt_ns` field was never invalidated after its target was
freed), or (b) a `struct mnt_namespace` layout/ABI mismatch between compilation units. Ruled out so far:
`alloc_mnt_ns()` unconditionally and explicitly sets `new_ns->seq = anon ? 0 : atomic64_add_return(...)`
right after `kmalloc()` (not `kzalloc()`, but the assignment is explicit, not relying on zeroed memory) —
so the corruption happens sometime *after* creation, before the mount's fd is closed.

Also tried this session, confirmed **not sufficient on its own**: added `namespace_lock()` (the
`namespace_sem` rwsem) around `kaos_attach_anon_ns()`'s tree-walk-and-assign loop
(`fs/namespace.c:3723`), matching mainline's `open_detached_copy()` which holds both `namespace_sem` and
`lock_mount_hash()` there (this backport's `open_tree()`/`fsmount()` both release `namespace_lock()`
*before* calling `kaos_attach_anon_ns()`, so the loop was previously running with zero `namespace_sem`
protection — a real, still-worth-keeping fix, since `next_mnt()` walks tree-topology links that every
other mount-tree-mutating function serializes on `namespace_sem` for). Rebuilt and retested: crash count
went *up* (11 oopses vs. 6 the run before), so either this fix is irrelevant to the real bug, or it
surfaced the real bug more often by changing timing. **This change is committed nowhere — still sitting
uncommitted in the working tree along with the diagnostics below.**

---

## 4. Current uncommitted state — read this before your next build

`fs/namespace.c` has uncommitted changes on top of `0c23db9f8180` (`git diff --stat` shows only this file,
~26 lines added). Contents, in order:

1. The `namespace_lock()`/`namespace_unlock()` addition in `kaos_attach_anon_ns()` (§3, not yet proven to
   help, possibly irrelevant).
2. A `KAOS_MNTAPI_LOG` line at the end of `kaos_attach_anon_ns()`, logging `root=%p m=%p ns=%p seq=%llu`
   right after the anon namespace is created and assigned — this is the creation-time counterpart to the
   dissolve-time log below, meant to let you compare "value at birth" vs. "value at dissolve" for the
   same object across a boot's log and determine whether the corruption happens immediately (a creation
   bug) or sometime during the mount's active lifetime (a UAF/external-corruption bug).
2. A `KAOS_MNTAPI_LOG` line + `dbg_seq` local in `kaos_dissolve_detached_mnt()`, logging
   `mnt=%p m=%p ns=%p seq=%llu anon=%d` right before the anon-check branch.

**This has been built and boot-tested once already** (kernel `#59`, `g0c23db9f8180-dirty`) but that test
only captured the *dissolve-time* log (item 2) before the diagnostic in item 1 (creation-time) was added —
so we don't yet have a same-boot creation-vs-dissolve comparison. **Next step: rebuild with the current
working tree as-is, reboot into Kali, pull `kaos-boot-kmsg.log`, and grep for `attach_anon` and
`dissolve mnt=` lines.** Match them up by the `ns=%p` hash (the `%p` hashed-pointer salt is stable within
one boot, so the same real object prints the same hash both times) and see whether `seq` is already wrong
at the `attach_anon` line, or only goes wrong by the time `dissolve` reads it.

If `seq` is already wrong at `attach_anon` time: the bug is in `alloc_mnt_ns()`/`kaos_attach_anon_ns()`
itself (a bounds/aliasing bug local to this file — check whether `next_mnt(p, mnt)`'s tree walk is
visiting more nodes than expected, e.g. an already-attached mount whose `mnt_mounts`/`mnt_child` links
alias into a live tree, and the loop is scribbling `mnt_ns` onto mounts it shouldn't touch).

If `seq` is fine at `attach_anon` time but wrong at `dissolve` time: it's a genuine UAF — something is
freeing this exact `struct mnt_namespace` (or overwriting its memory) between creation and the fd's
close. Suspects worth checking first: (a) a second, unexpected `free_mnt_ns()` call on the same `ns` from
somewhere (check the `move_mount()` `old_anon_ns` cleanup added in `0c23db9f8180` for a
double-free/list-corruption edge case — e.g. what happens if `move_mount()` is attempted, fails partway,
and `kaos_dissolve_detached_mnt()` *also* later fires on the same fd's close); (b) percpu allocator /
slab reuse timing if `mnt_pcp` (this kernel's `CONFIG_SMP` per-mount refcount, allocated in
`alloc_vfsmnt()`) is somehow involved — the crash register dump in earlier oopses showed the fault address
matching a value that looked like it came from `mnt_add_count()`'s percpu dereference, which is a
different (but possibly related) angle from the `ns->seq` corruption; worth checking whether `mnt`
itself (not just `ns`) is also going stale.

**None of this diagnostic-only working-tree state has been committed.** Don't commit it as-is without at
least the `namespace_lock()` change being separated out and independently justified — it's unproven and
may be a red herring, and mixing it with pure diagnostics in one commit will make future bisection harder.

---

## 5. How to test / where the logs are

Standard cycle used all session, entirely via `adb` (device must be USB-connected, no live shell needed):

```bash
adb root && adb remount     # if needed
# Clear logs before a test:
adb shell "rm -f /data/.stowaway/kali/var/log/boot.log \
  /data/.stowaway/kali/var/log/kaos-mountinfo-precommit.log \
  /data/.stowaway/kali/var/log/kaos-boot-kmsg.log \
  /data/.stowaway/kali/var/log/kaos-coldboot.log
  : > /data/adb/kaos-service.log"

# Build + flash boot.img with your kernel changes, then reboot into Kali as usual (see main CLAUDE.md).

# After the crash-loop settles (device typically falls back to Android):
adb shell "wc -l /data/.stowaway/kali/var/log/kaos-boot-kmsg.log"
adb shell "head -2 /data/.stowaway/kali/var/log/kaos-boot-kmsg.log"   # confirms kernel build/commit string
adb shell "grep -n -E 'Kernel BUG|Internal error|Oops|ida_remove called' /data/.stowaway/kali/var/log/kaos-boot-kmsg.log"
adb shell "grep -n 'kaos-mount-api\|attach_anon\|dissolve mnt=' /data/.stowaway/kali/var/log/kaos-boot-kmsg.log"
```

**Never use `dmesg`** — it's cleared across reboots and was the reason earlier sessions couldn't make
progress on this exact bug. `kaos-boot-kmsg.log` is the disk-backed, persistent equivalent and is what
every finding in this document is based on.

The kernel version string in the log's first two lines (`Linux version 4.9.337-perf-g<hash>[-dirty] ...
#<build-number> ...`) tells you exactly which commit/working-tree-state produced that boot — always check
this before trusting a log's findings against your current tree.

---

## 6. Quick pointers

- `kernel/xiaomi/sdm845/fs/namespace.c` — everything in this handoff. Key functions: `clone_mnt()` (1048),
  `attach_recursive_mnt()` (2127), `free_mnt_ns()` (3003), `alloc_mnt_ns()` (3031), `kaos_attach_anon_ns()`
  (3723), `kaos_dissolve_detached_mnt()` (3779), `fsmount()`/`open_tree()` syscalls further down.
- `kernel/xiaomi/sdm845/fs/mount.h` — `is_anon_ns()` (line 34), `struct mnt_namespace` definition.
- `KAOS_MNTAPI_LOG` macro — top of `fs/namespace.c`, plain `printk(KERN_INFO ...)`, lands in the same
  persistent `kaos-boot-kmsg.log`. Intended to be removed once root-caused; still needed for now.
- Git: kernel repo `kamstartech/android_kernel_xiaomi_sdm845`, branch `lineage-22.2-nethunter`, currently
  2 commits ahead of the tracked remote (`34c83c3ed41b`, `0c23db9f8180`), not pushed. Uncommitted
  diagnostic/namespace_lock changes in the working tree per §4.
- Build convention: the user builds/flashes personally (`build/soong/soong_ui.bash --make-mode bootimage`
  etc., see main `CLAUDE.md`) — don't run `mka`/`soong_ui.bash`/kernel `make` unless explicitly asked.
