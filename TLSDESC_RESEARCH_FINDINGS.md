# TLSDESC Research Findings — Halium Boot Mode for Kaos/Perseus

Research date: 2026-08-01. Research-only task; no files were modified.

Scope: Investigate whether the local AOSP tree at ~/hadk can be rebuilt to avoid
AArch64 TLSDESC (TLS descriptor) relocations — the "rebuild bionic pieces with
-mtls-dialect=trad" idea — as an alternative to fixing libhybris's TLSDESC
support. Also surveyed current upstream libhybris state.

Bottom line up front:

1. **`-mtls-dialect=trad` is NOT usable with this tree's clang.** The prebuilt
   Android clang (clang-r536225, clang 19.0.1) rejects `-mtls-dialect=` for any
   aarch64 target with `clang: error: unsupported option`. Clang has no
   AArch64 TLS-dialect selection flag; the bionic docs themselves state "Clang
   always uses the default mode" (bionic/docs/elf-tls.md line 328). The premise
   of the idea is therefore not directly implementable with the shipped
   toolchain.

2. **The real blocker is not actually unresolved.** The libhybris "q" generation
   TLSDESC support — the thing that was deliberately compiled out via
   `DISABLED_FOR_HYBRIS_SUPPORT` upstream and which prints "TLS relocations not
   yet implemented in libhybris" — has been **implemented and merged upstream
   (PR #560, commit f306111, landed Aug 2024)**, and that implementation is
   **already present and buildable in BOTH local libhybris trees** in this repo
   (`external/libhybris` and `hybris/mw/libhybris`). The on-device q.so that
   printed the fatal message is a stale/older build. A freshly built q.so from
   this tree already contains the full TLSDESC resolver set
   (`tlsdesc_resolver_static/dynamic/dynamic_slow_path/unresolved_weak`).

3. **The compiled-out `-mtls-dialect=trad` idea has an even deeper landmine**:
   the Adreno/vendor GPU blobs on this device are closed-source prebuilts. Even
   if one could force `trad` for all AOSP-built code, the vendor blobs would
   still carry whatever TLS model Qualcomm compiled into them, and those cannot
   be rebuilt. Additionally, the local bionic tree already carries a TLS-slot
   realignment patch (`tls_defines.h` under `HYBRIS_BUILD`) specifically to
   keep *inlined* bionic TLS-slot accesses compatible with the blobs — a
   separate problem from relocations. Forcing trad only on ld-android.so would
   not remove the need for the libhybris linker to process TLS relocations in
   the *libraries* it loads.

Full details below, with exact file/line citations.

---

## 1. Does this AOSP tree control the AArch64 TLS access model?

### 1a. `-mtls-dialect=` is absent from all build config

Searched the entire tree for `-mtls-dialect` / `mtls`:

- `bionic/docs/elf-tls.md` (lines 327-329) is the ONLY place the flag is
  mentioned, and it is documentation, not build config:
  - Line 327-328: "GCC can select the design at run-time using
    `-mtls-dialect=<dialect>` (trad-vs-desc on arm64, otherwise gnu-vs-gnu2).
    **Clang always uses the default mode.**"
  - Line 329: "GCC and Clang default to TLSDESC on arm64 and the traditional
    design on other architectures."
- No occurrences in any `Android.bp`, `Android.mk`, `BoardConfig.mk`, or soong
  `config/*.go` file.

### 1b. The prebuilt clang version and its empirical TLS behavior

Default clang version:
- `build/soong/cc/config/global.go:390` → `ClangDefaultVersion = "clang-r536225"`
- `build/soong/cc/config/global.go:391` → `ClangDefaultShortVersion = "19"`
- Binary reports: `clang version 19.0.1` (Android r536225), LLVM 19.0.1.

Empirical tests performed with this exact prebuilt (`prebuilts/clang/host/linux-x86/clang-r536225/bin/clang`), using a `__thread` test file and `llvm-readelf -r`:

| Invocation | Result |
|---|---|
| `--target=aarch64-linux-android -fPIC -c` (default) | emulated TLS: `__emutls_v.gx` + `__emutls_get_address` |
| same + `-fno-emulated-tls` | **`R_AARCH64_TLSDESC_*` relocations (TLSDESC model)** |
| `--target=aarch64-linux-gnu -fPIC -c` (generic Linux) | `R_AARCH64_TLSDESC_*` |
| `--target=aarch64-linux-android -mtls-dialect=trad` | **`clang: error: unsupported option '-mtls-dialect=' for target 'aarch64-unknown-linux-android'`** |
| `--target=aarch64-linux-gnu -mtls-dialect=trad` | **unsupported option** (same) |
| `--target=aarch64-linux-gnu -mtls-dialect=desc` | **unsupported option** (same) |
| `--target=x86_64-linux-gnu -mtls-dialect=gnu/gnu2` | accepted (x86 supports the flag) |

Conclusions:
- Clang 19 for AArch64 has **no TLS dialect selection flag at all**. The
  "rebuild with `-mtls-dialect=trad`" idea cannot be expressed with the shipped
  clang. (Upstream clang exposes `-mtls-dialect=` only for X86 targets in this
  version.)
- When real ELF TLS is enabled, clang 19 emits **TLSDESC** on aarch64 by
  default. The android target defaults to *emulated* TLS unless ELF TLS is
  explicitly enabled.

### 1c. How ELF TLS is actually enabled (and that the shipped image uses TLSDESC)

- `-fno-emulated-tls` does NOT appear anywhere in `build/soong/` (only one
  comment in `build/soong/cc/sanitize.go:853` about sancov emulated TLS), nor
  in `bionic/*/Android.bp`. Soong's global flag lists
  (`build/soong/cc/config/global.go`, `arm64_device.go`, `clang.go`,
  `toolchain.go`) contain no TLS flags at all.
- Despite that, the empirically built binaries confirm the device image uses
  real ELF TLS with TLSDESC. Checked actual build outputs under
  `out/soong/.intermediates/bionic/`:
  - `libc_malloc_debug.so`
    (`.../libc/malloc_debug/libc_malloc_debug/android_arm64_armv8-a_shared_apex10000/.../libc_malloc_debug.so`)
    contains a real `R_AARCH64_TLSDESC` dynamic relocation.
  - No `__emutls_v.` symbols found in any built bionic `.o`/`.so` for arm64.
- So in practice the entire AOSP system image is compiled with ELF TLS and the
  AArch64 **TLSDESC** model is in active use in shipped libraries. The mechanism
  that injects `-fno-emulated-tls` is not visible in the config Go files I
  inspected (it may be a target/triple default or an injected flag level not
  present in `cc/config/*.go`), but the empirical binary evidence is conclusive
  and matches the bionic docs.

### 1d. bionic/linker and bionic/libc TLS cflags

- `bionic/linker/Android.bp`:
  - `linker_bin_template` (line 265) already carries a local **`-DHYBRIS_BUILD`**
    cflag (line 273) — this repo already builds the linker with hybris-specific
    code paths.
  - arm64 sources include `arch/arm64/tlsdesc_resolver.S` (line 216) and
    `linker_tls.cpp` (line 198) — the real TLSDESC resolver machinery is
    compiled into the linker.
  - `linker_bin_template` ldflags include `-Wl,-soname,ld-android.so` (line 305).
  - **No `-mtls-dialect` and no TLS-related cflags beyond `-DHYBRIS_BUILD`.**
- `bionic/libc/Android.bp`: no TLS-model cflags present (searched; nothing
  beyond the common `libc_common_flags` block at lines 20-49).

---

## 2. What is `ld-android.so` and where is it built from?

There are TWO distinct things named `ld-android.so` in this tree:

1. **The real dynamic linker** (`/system/bin/linker64` and, inside the runtime
   APEX, `com.android.runtime`'s `linker64`). Built from the `cc_binary
   "linker"` module (`bionic/linker/Android.bp:357`) via
   `linker_bin_template` (line 265), which links all of `bionic/linker/*.cpp`
   (linker_tls.cpp, linker_relocate.cpp, tlsdesc_resolver.S, etc.) with
   `-Wl,-soname,ld-android.so`. This is a 2.2 MB binary
   (`out/.../obj/EXECUTABLES/linker.com.android.runtime_intermediates/linker64`,
   2183880 bytes) containing `__dl_tlsdesc_resolver_*` symbols and only
   `R_AARCH64_IRELATIVE` relocations (correct: bionic cannot use ELF TLS inside
   the loader itself).

2. **A tiny stub** `cc_library "ld-android"` (`bionic/linker/Android.bp:432`)
   built from a single 62-line `ld_android.cpp` that aliases every
   `__loader_*` symbol to a trapping `__internal_linker_error()`. It does NOT
   use `linker_bin_template`. Its build output is the small 9512-byte
   `/system/lib64/ld-android.so` seen in the out tree. This stub is a libdl
   interface shim (used to satisfy linking against `ld-android`), not the
   loader itself.

**Conclusion for the task:** the loader binary that libhybris's q shim dlopens
as "ld-android.so" is the full linker built from `bionic/linker/*` sources in
this tree — it is NOT a vendor prebuilt. It already compiles the bionic TLSDESC
resolver (`arch/arm64/tlsdesc_resolver.S`, `linker_tls.cpp`). Whether the
specific `/android/system/lib64/ld-android.so` path libhybris loads resolves to
the full linker or the stub depends on the deploy layout, but both are built
from this source tree.

---

## 3. Global vs. scoped flag to force trad TLS

- **Global:** There is no supported global flag. Since clang rejects
  `-mtls-dialect=` for aarch64, there is nothing to append to
  `BoardConfig.mk`/`device/xiaomi/perseus/*.mk` or soong global cflags that
  would select `trad`. Any such flag would be a clang error on every aarch64
  compile.
- **Scoped:** Even a scoped per-module `cflags: ["-mtls-dialect=trad"]` in
  `bionic/linker/Android.bp` would hit the same "unsupported option" error.
  There is no supported way to make this clang emit traditional GD accesses on
  aarch64.
- **What could be done instead (all non-trivial, none a simple flag):**
  - Prebuilt clang is what it is; switching to GCC's `-mtls-dialect=trad`
    (which GCC does support on aarch64) is not how AOSP builds and would
    reintroduce a GCC toolchain AOSP abandoned. Not realistic.
  - Using `-fno-emulated-tls` removal / emulated TLS (the android target's
    *default*) WOULD avoid ELF TLS entirely — but emulated TLS is slow,
    changes the ABI (`__emutls_v.*` symbols), and libhybris is built around
    ELF TLS assumptions; not a viable system-wide switch.
  - The realistic "avoid TLSDESC relocations" lever is to accept that
    libhybris must handle TLSDESC — which, see section 4, upstream already
    implemented.

---

## 4. libhybris upstream: issues #481 and #559, and the landed fix

Network/gh access was available; verified live.

### Issue #481 — "android api 29 undefined symbol: tlsdesc_resolver_dynamic" (OPEN)
- Author exhoty, opened 2021-04-01, 1 comment. State: **OPEN**.
- The lone comment (member krnlyng) is the `#ifdef DISABLED_FOR_HYBRIS_SUPPORT`
  workaround diff around the `deferred_tlsdesc_relocs` loop — exactly the code
  quoted in the task.

### PR #560 — "hybris: common: q: Implement tls relocation" (MERGED)
- Author luka177, merged Aug 2024 (commit `f306111`), +2025/-54, reviewed by
  mlehtima. **This is the real fix.** It:
  - Removed every `"TLS relocations not yet implemented in libhybris"`;
    `abort();` from `hybris/common/q/linker.cpp` and replaced them with full
    R_GENERIC_TLS_* and R_GENERIC_TLSDESC handling (verify: the diff removes
    the abort blocks at the STT_TLS / is_tls_reloc / R_GENERIC_TLS_TPREL /
    DTPMOD / TLSDESC cases).
  - Added a complete `tlsdesc_resolver.S` (aarch64) with
    `tlsdesc_resolver_static`, `tlsdesc_resolver_dynamic`,
    `tlsdesc_resolver_dynamic_slow_path`, `tlsdesc_resolver_unresolved_weak`,
    plus a bionic port (`bionic_elf_tls.cpp`, `bionic_allocator.cpp`,
    `bionic_tls.h`, `tls_defines.h`) so `__tls_get_addr` works inside q.so.
- **This means the "hard unsolved upstream problem" named in the task has
  already been solved and merged upstream.**

### Issue #559 — "Saving/restoring TLS pointer before and after entering bionic functions?" (OPEN, 29 comments)
- Opened 2024-07-14 by twaik. State: **OPEN**.
- Discussion timeline (most recent comments fetched):
  - 2025-01-23: GranPC actively experimenting with runtime TLS-pointer swapping
    via generated wrappers; reports bionic→glibc callbacks (e.g. GL init calling
    malloc) still crash because the new TLS block doesn't match glibc's view.
  - krnlyng suggests dynamic wrapper-code generation, handle start_routine,
    function pointers, HYBRIS_IMPLEMENT_FUNCTION macros.
  - twaik points at `lindroid-quirks` (Linux-on-droid) TLS padding approach
    and notes "patching bionic ... will require rebuilding some components that
    are using TLS, like libEGL.so".
  - 2025-01-24: krnlyng floats runtime scanning+code-patching of TLS accesses.
  - Last comment 2025-01-30. **Still open, no landed TLS-pointer-swap
    solution.** This work is orthogonal to TLSDESC relocation handling; it is
    about glibc/bionic TLS-area sharing, not about whether TLSDESC relocations
    can be processed.

### PR #575 — "hybris: experimental TLS access patcher for aarch64" (OPEN, not merged)
- Author NotKit, last updated 2025-08-31. `HYBRIS_TLS_PATCH=1` (or a
  colon-separated library list) scans loaded code for the inlined pattern
  `mrs x<rt>, tpidr_el0; ldr/str [x<rt>,#offset]` and rewrites the offset to a
  libhybris-owned TLS area — targeting the **inlined private bionic TLS-slot
  accesses** in closed drivers (noticed with libGLES_mali.so). It is
  explicitly "initial attempt", "not reliable", and UNMERGED. This addresses a
  different failure mode than relocation processing, and is directly relevant
  to the closed-vendor-blob concern in the task.

### Net assessment
- **TLSDESC relocation handling is DONE upstream and in this repo.**
- **TLS-area sharing between glibc and bionic (issues #559/#575) is still open.**
  That is the true "unsolved architectural problem" today, but it is a separate
  problem from the one the task described ("TLS relocations not yet
  implemented"). It matters for the *inlined TLS-slot accesses* in vendor
  blobs (Adreno), which cannot be fixed by any compile flag on AOSP side.

---

## 5. Vendor blobs (Adreno) and TLS

- The device's closed-source Adreno blobs are in
  `out/target/product/perseus/vendor/lib64/egl/`:
  `libEGL_adreno.so`, `libGLESv2_adreno.so`, `libGLESv1_CM_adreno.so`,
  `libq3dtools_adreno.so`, `eglSubDriverAndroid.so`, etc. They use packed
  `DT_ANDROID_RELA` relocations (reloc sections show `ANDROID_RELA`), which
  `llvm-readelf` does not decode into per-type relocations; my manual packed
  decode was inconclusive (marked unconfirmed).
- No undefined TLS symbols (`__tls_get_addr`, `tlsdesc_resolver_*`) appear in
  their dynamic symbol tables, which is suggestive but not conclusive.
- **Unconfirmed:** whether these specific Adreno blobs contain TLSDESC or
  inlined bionic TLS-slot relocations. The memory-indexed prior sessions in
  this repo (session 682e3921) explicitly documented a conflict between the
  local `tls_defines.h` slot reshuffle and the **Adreno GPU driver**, i.e. the
  driver DOES reach into fixed bionic TLS slots. That is the inlined-slot
  problem (the one PR #575 attacks), not the relocation problem.
- Because these blobs are prebuilt and cannot be recompiled, forcing `trad` on
  AOSP code cannot make the blobs consistent with any given TLS model. Any
  libhybris/TLS fix must interoperate with the blobs as-shipped.

---

## 6. What this repo already contains (the key practical finding)

Both libhybris checkouts in this repo already carry the merged TLSDESC
implementation, plus additional local hybris hardenings:

1. `external/libhybris` (git branch `lineage-22.2-nethunter`, HEAD `4454c78`
   dated 2026-07-10):
   - Commit `f306111` (PR #560) is an ancestor of HEAD.
   - `hybris/common/q/linker.cpp:3450-3457`: the aarch64
     `deferred_tlsdesc_relocs` → `tlsdesc_resolver_dynamic` loop is ACTIVE
     (NOT wrapped in `DISABLED_FOR_HYBRIS_SUPPORT`).
   - `hybris/common/q/tlsdesc_resolver.S`: full aarch64 resolver set (203
     lines).
   - `hybris/common/q/bionic/libc/bionic/bionic_elf_tls.cpp`,
     `bionic_elf_tls.cpp`, `bionic_allocator.cpp`: bionic TLS runtime ported in.
   - `-DHYBRIS_BUILD` and `tlsdesc_resolver.S` are wired into
     `hybris/common/q/Makefile.am` (lines 45-50, 61).

2. `hybris/mw/libhybris/libhybris` (the checkout that actually builds the
   deployable Ubuntu Touch packages; HEAD `610f19e`):
   - `f306111` is an ancestor; `tlsdesc_resolver.S` present.
   - A **built** `hybris/common/q/.libs/q.so` (dated 2026-07-10 19:41) already
     exports `tlsdesc_resolver_static`, `tlsdesc_resolver_dynamic`,
     `tlsdesc_resolver_dynamic_slow_path`, `tlsdesc_resolver_unresolved_weak`,
     and `__tls_get_addr` (verified with llvm-readelf).

3. Local bionic modifications (git status in `bionic/`, all uncommitted):
   - `libc/platform/bionic/tls_defines.h`: under `HYBRIS_BUILD`, keeps the
     standard arm64 slots (APP=2, OPENGL=3, OPENGL_API=4, STACK_GUARD=5,
     SANITIZER=6, ART_THREAD_SELF=7) and appends `TLS_SLOT_HYBRIS=8`
     (`MAX_TLS_SLOT=8`); non-HYBRIS builds keep stock layout. This is the
     "Safe TLS Realignment" from prior sessions.
   - `libc/Android.bp`, `libdl/Android.bp`, `linker/Android.bp`: `-DHYBRIS_BUILD`
     additions; `libc` adds `hybris_support.c`; `libc.map.txt` exports
     `__get_tls_hooks`.
   - `libc/bionic/gwp_asan_wrappers.cpp`, `libc/system_properties/prop_area.cpp`,
     `libdl/libdl_cfi.cpp`, `linker/linker_cfi.cpp`: hybris-guarded
     workarounds (disable GWP-ASan / CFI shadow under hybris).

**Conclusion:** a complete, buildable TLSDESC fix already exists in this source
tree. The on-device q.so that printed "TLS relocations not yet implemented in
libhybris" is stale relative to this tree. The immediate, low-risk action is
NOT a compiler-flag rebuild of bionic; it is to rebuild/redeploy the libhybris
q.so from this tree (or from the current upstream master, which has the same
fix) so the device actually runs the TLSDESC-capable linker.

---

## 7. Recommendation

**Do NOT pursue the `-mtls-dialect=trad` rebuild path.** It is not implementable
with the shipped clang (flag rejected on aarch64), it is not what the blocker
needs, and it cannot fix closed vendor blobs that already carry their own TLS
behavior.

Instead:

1. **First, redeploy the libhybris q.so already built in this tree.**
   `hybris/mw/libhybris/libhybris/hybris/common/q/.libs/q.so` already contains
   the full TLSDESC resolver set. Rebuilding the Ubuntu Touch libhybris package
   (and/or updating the rootfs) from this source, then re-running the Halium
   boot, is the most likely way to clear the "TLS relocations not yet
   implemented" fatal and get past ld-android.so linking. Verify with
   `HYBRIS_LD_DEBUG=1` that q.so no longer aborts at the TLSDESC stage.

2. **Treat the remaining problems as the *glibc/bionic TLS-area sharing*
   problem, not the relocation problem.** If the boot then dies on inlined
   bionic TLS-slot accesses inside the closed Adreno blobs (the failure mode
   documented in prior sessions and by PR #575), the open items are:
   - upstream PR #575's `HYBRIS_TLS_PATCH` patcher (experimental, unmerged), or
   - GranPC/krnlyng's TLS-pointer swapping work (#559, still open), or
   - the existing local `tls_defines.h` "Safe TLS Realignment" +
     `__get_tls_hooks` approach already in this tree, whose correctness vs. the
     Adreno driver must be validated (prior session 682e3921 flagged a
     conflict here).

3. **Only if a genuine reason appears to need *no TLSDESC at all* in AOSP-side
   code** would rebuilding specific pieces with emulated TLS or a GCC toolchain
   be worth considering — and that is a large, ABI-changing, risky
   undertaking with no clear benefit given that TLSDESC relocation handling is
   already merged in libhybris.

4. **Recheck on-device provenance.** Before any rebuild, confirm whether the
   deployed q.so is older than commit f306111 (Aug 2024). If the deploy source
   is pinned to an old Ubuntu Touch release, updating libhybris to >= that
   commit is the fix.

---

## Appendix: exact citations

- Default clang: `build/soong/cc/config/global.go:390-391`
- TLS dialect docs: `bionic/docs/elf-tls.md:327-329`
- linker module / soname: `bionic/linker/Android.bp:265,273,305,357,432`
- linker arm64 sources: `bionic/linker/Android.bp:212-219` (tlsdesc_resolver.S
  at 216), `linker_tls.cpp` at 198
- ld-android stub: `bionic/linker/ld_android.cpp` (62 lines), module at
  `bionic/linker/Android.bp:432`
- local bionic diffs: `git -C bionic status` (tls_defines.h, gwp_asan,
  prop_area, libdl_cfi, linker_cfi, libc.map.txt, Android.bp files)
- libhybris TLS impl commits: `f306111` (PR #560), `044364f`,
  `43621a5` (allocator free fix, Halium 14 tested)
- q/linker.cpp active TLSDESC loop: `hybris/common/q/linker.cpp:3450-3457`
- q resolver assembly: `hybris/common/q/tlsdesc_resolver.S`
- built q.so symbol proof:
  `hybris/mw/libhybris/libhybris/hybris/common/q/.libs/q.so`
- empirical clang rejection of `-mtls-dialect=trad` on aarch64: reproduced
  with prebuilt clang-r536225 (19.0.1)
- empirical TLSDESC in shipped image: `R_AARCH64_TLSDESC` present in built
  `libc_malloc_debug.so`
- upstream issues/PRs (verified live via gh): #481 OPEN, #559 OPEN (last
  comment 2025-01-30), #560 MERGED, #575 OPEN (updated 2025-08-31)

## Appendix: unconfirmed / not fully checked items

- Exact TLS relocation types inside the closed Adreno blobs
  (`libEGL_adreno.so`, `libGLESv2_adreno.so`): their packed ANDROID_RELA
  sections were not reliably decoded; the per-type contents are UNCONFIRMED.
- The precise soong mechanism that injects `-fno-emulated-tls` (not found in
  `build/soong/cc/config/*.go`; inferred from built binaries + bionic docs).
- On-device q.so provenance/version (no live device access).
