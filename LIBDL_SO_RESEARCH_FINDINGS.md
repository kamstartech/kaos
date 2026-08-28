# libdl.so "not found" Research Findings — Kaos/Perseus Halium Boot Mode

Research date: 2026-08-01. Research-only task; no files were modified.

Scope: Follow-on to the TLSDESC fix. After deploying a freshly-built
`hybris/mw/libhybris/libhybris` q.so + libhybris-common.so to the Ubuntu Touch
device, the boot log shows `library "libdl.so" not found` immediately after
`[ Reading linker config "/system/etc/ld.config.txt" ]`, which cascades into
libcutils.so failing to load (`failed to load bionic libc.so, falling back own
property implementation`) and finally `eglInitialize failure:
EGL_NOT_INITIALIZED`.

## Bottom line

1. **Root cause is NOT the missing linker config, and is NOT a namespace /
   permission gate.** On a config read failure the q linker falls back to a
   *permissive* non-isolated default namespace
   (`linker.cpp:4233-4249`), which is exactly what old builds used. A stub
   `/system/etc/ld.config.txt` (candidate fix "a") will **not** fix this
   problem by itself.

2. **Root cause is a project-local regression in the deployed q.so.** Commit
   `97bd37c` (2026-07-10 19:25, this project's own commit) added an
   unconditional skip of `/bootstrap` search paths for `libdl.so` in
   `open_library_on_paths()` at
   `hybris/common/q/linker.cpp:1155-1161`:

   ```c
   if (strcmp(name, "libdl.so") == 0 &&
       path.size() >= 10 && path.rfind("/bootstrap") == path.size() - 10) {
     continue;
   }
   ```

   On this device the **only** real libdl.so lives at
   `/system/lib64/bootstrap/libdl.so` (confirmed live). The built q.so in this
   tree (timestamp 2026-07-10 19:41, i.e. after 97bd37c) provably contains the
   skip (`/bootstrap` string present in `hybris/common/q/.libs/q.so`). So the
   deployed linker deliberately refuses the one file that can satisfy
   `DT_NEEDED libdl.so`, and no other path on the device contains libdl.so
   (the old `/system/lib64/libdl.so` is an APEX symlink whose target is not
   present when UT boots). Every library that DT_NEEDEDs libdl.so (libcutils,
   libEGL, etc.) therefore fails.

3. **The skip's stated rationale is obsolete in this build.** The comment says
   the bootstrap libdl.so "is a stub that lacks CFI helpers such as
   `__cfi_init`". But this project has already neutralized the entire CFI path
   under `HYBRIS_BUILD`:
   - q-side shadow init is compiled out (`linker_cfi.cpp:230-239`), so the
     linker never calls `__cfi_init`;
   - bionic-side `libdl/libdl_cfi.cpp:36-46,78-82` makes `__cfi_init` a
     no-op store and neutralizes the CFI slowpath;
   - no other code in the tree calls `__cfi_init` (searched; none).
   So the skip now serves no purpose except to block the device's only libdl.so.

4. **Recommended fixes (in order of preference), all consistent with the
   source logic:**

   - **(Primary) Remove / revert the bootstrap-skip**
     (`linker.cpp:1155-1161`, commit `97bd37c`). This restores the exact,
     previously-working resolution path
     (`/system/lib64/bootstrap/libdl.so` via `HYBRIS_LD_LIBRARY_PATH`,
     which is unchanged). Lowest risk; it returns to the behavior of the old
     stock q.so.
   - **(Alternative, matches real Android) Make the linker-provided libdl
     actually satisfy `DT_NEEDED "libdl.so"`** by setting the synthetic
     libdl soinfo's soname to `"libdl.so"` in
     `get_libdl_info()` (`hybris/common/q/dlfcn.cpp:378`). The host-ELF fix
     from the TLSDESC task already gives that soinfo valid symbol tables
     (the `q.so soinfo populated from host ELF` log line), so this makes
     `find_loaded_library_by_soname("libdl.so")` match it and libdl resolves
     from q.so's own symbols — exactly how modern bionic treats libdl.so
     (linker-provided since API 28+). No real libdl.so file is then needed at
     all. Slightly larger behavioral change than (1) but semantically cleaner.
   - **(Rootfs-only, no code change)** Materialize a real libdl.so at a
     non-`/bootstrap` path the search already covers, e.g. make
     `/system/lib64/libdl.so` a real file (copy/bind of
     `/system/lib64/bootstrap/libdl.so`) so the dangling APEX symlink is
     satisfied. Works, but leaves the code landmine in place and only patches
     this one library.

Below: answers to the four task questions with exact citations, then the
evidence trail.

---

## Q1. Is "libdl.so" special-cased in the q linker? If so, why does the
host-ELF fix not make it work?

**Yes, it is special-cased in three places — but none of them makes
`DT_NEEDED "libdl.so"` resolve to the linker's own symbols.**

### (a) The bootstrap-path skip — `hybris/common/q/linker.cpp:1155-1161`
In `open_library_on_paths()` (the per-path file-open loop used by
`open_library()` at `linker.cpp:1177-1227`), any search path whose name ends
in `/bootstrap` is skipped when the library is `libdl.so`. This is the
proximate blocker: on this device the only real libdl.so is at
`/system/lib64/bootstrap/libdl.so`, and this skip prevents it from ever being
opened. Git blame shows it was added by project-local commit `97bd37c`
(2026-07-10), not by upstream.

### (b) The synthetic "linker-provided" libdl soinfo — `hybris/common/q/dlfcn.cpp:356-387`
`get_libdl_info(kLinkerPath, tmp_linker_so)` builds a synthetic soinfo for
libdl from the linker's own ELF symbol tables (this is the API-28+ "libdl is
provided by the linker" mechanism):

```
dlfcn.cpp:360  __libdl_info = new (...) soinfo(g_default_namespace, linker_path, nullptr, 0, 0);
dlfcn.cpp:362  __libdl_info->strtab_ = linker_si.strtab_;
dlfcn.cpp:363  __libdl_info->symtab_ = linker_si.symtab_;
...
dlfcn.cpp:378  __libdl_info->soname_ = linker_si.soname_;
```

`linker_si` is `tmp_linker_so` from `android_linker_init()`
(`linker_main.cpp:891-896`), which `generate_tmpsoinfo()` populates from the
host ELF (libhybris-common.so). With the TLSDESC-era host-ELF fix the symbol
tables are now valid (that is what the `q.so soinfo populated from host ELF`
log line confirms). **But** `linker_si.soname_` is the host DSO's soname
`libhybris-common.so.1` (verified via `readelf -d` on the built
`libhybris-common.so.1.0.0`), **not** `libdl.so`. Consequently
`find_loaded_library_by_soname("libdl.so")` (`linker.cpp:1511-1523`, called
from `find_library_internal` at `linker.cpp:1604`) never matches it.

**Why the host-ELF fix doesn't help libdl.so:** the fix made the synthetic
soinfo's *symbol tables* valid, which is exactly what is needed for the
mechanism to work "in principle" — but there is **no name-based path** that
maps the string `"libdl.so"` onto that soinfo. Resolution of a `DT_NEEDED
libdl.so` goes: (1) `find_loaded_library_by_soname` → no match (wrong soname);
(2) `load_library` → `open_library` → real file search → bootstrap path
skipped by (a) → nothing else exists → `DL_ERR("library \"%s\" not found")`
at `linker.cpp:1501`. So libdl.so still has to be found as a *file*, and the
only file is skipped.

### (c) The "hybris we have no libdl soinfo" comments — `linker.cpp:4410-4425`
The upstream code that injects ld-android.so / libdl into every namespace is
commented out with exactly this note. `init_default_namespaces()` never
registers any soinfo named `libdl.so` in any namespace.

### Also: CFI libdl lookup — `hybris/common/q/linker_cfi.cpp:137-156`
`find_libdl()` explicitly skips the synthetic soinfo (realpath won't contain
`/libdl.so`) and additionally requires `__cfi_init` in the matched soinfo. It
was written to find a *real, mapped* `/system/lib64/libdl.so` as a dependency.
This reinforces that the design intent is "load a real libdl.so file", not
"resolve libdl.so from q.so".

---

## Q2. Does `linker_config.cpp` enforce a restrictive default on read failure?

**No — the no-config fallback is permissive, and it is not the cause of the
failure.**

- `[ Reading linker config "..." ]` is printed at `linker.cpp:4340` inside
  `init_default_namespaces()`.
- Config path selection is `get_ld_config_file_path()` (`linker.cpp:4277-4317`):
  `/linkerconfig/ld.config.txt` is probed first (the `Warning: failed to find
  generated linker configuration` INFO at `4306`), then
  `/system/etc/ld.config.txt` (`kLdConfigFilePath`, `linker.cpp:125`) is the
  final fallback. Neither exists when UT boots.
- On read failure: `parse_config_file()` (`linker_config.cpp:182-193`) returns
  false on `ENOENT` (empty error_msg); `Config::read_binary_config()`
  (`linker_config.cpp:450-460`) returns false; `init_default_namespaces()`
  sets `config = nullptr` (`linker.cpp:4352`) and calls
  `init_default_namespace_no_config()` (`linker.cpp:4233-4249`):
  - `set_isolated(false)` (permissive — no path restrictions),
  - default paths = `kDefaultLdPaths` (`linker.cpp:154-161`):
    `/system/lib64`, `/odm/lib64`, `/vendor/lib64`,
    `/apex/com.android.runtime/lib64`, `/apex/com.android.i18n/lib64`
    (none of which contain a real libdl.so on this device).
- The default-namespace `ld_library_paths` come from the parsed
  `HYBRIS_LD_LIBRARY_PATH` (`parse_LD_LIBRARY_PATH`, `linker.cpp:434-437`,
  called at `linker_main.cpp:873-876`), which does include
  `/system/lib64/bootstrap` — but the bootstrap-skip in Q1(a) suppresses that
  entry for libdl.so.

So a correctly-formed stub config could not grant access to a library that no
non-`/bootstrap` path contains. **The config file is a red herring for this
specific failure.** Note also that `linker_config.cpp` is byte-identical
between the two local trees.

### Why the per-path debug trace is absent (task observation)
With `HYBRIS_LD_DEBUG=1`, `g_ld_debug_verbosity = 1`
(`linker_main.cpp:861-864`). The per-path `TRACE("[ opening %s from namespace
%s from %s ]")` lines in `open_library` (`linker.cpp:1181,1209,1212,1222`) are
`_PRINTVF(1, ...)` = verbosity **>1** (`linker_debug.h:98`), so they are not
printed at level 1. The visible `[ Reading linker config ]` is `INFO`
(verbosity **>0**, `linker_debug.h:97`). So the absence of per-path traces is a
verbosity artifact, **not** evidence of an earlier/namespace-gate failure. The
`DL_ERR` at `linker.cpp:1501` proves the full file-search loop ran and returned
`-1`.

---

## Q3. Differences between `external/libhybris` and `hybris/mw/libhybris/libhybris`

Focused diff of the relevant files:

| Area | external/libhybris | hybris/mw/libhybris (deployed) |
|---|---|---|
| `linker.cpp` `open_library_on_paths` | **no** `/bootstrap` skip (line 1154 loops straight into `format_path`) | **has** the skip at 1155-1161 (commit 97bd37c) |
| `linker.cpp` `prelink_image` | no dynamic pre-set guard | has the host-ELF/dynamic pre-set guard (TLSDESC-era fix, 3520-3525) |
| `linker.cpp` rest | identical modulo `hybris_probe` debug logging | identical modulo debug logging |
| `linker_cfi.cpp` `find_libdl` | matches any soname `libdl.so` (135-138) | requires realpath contains `/libdl.so` **and** `__cfi_init` present (137-156) |
| `linker_cfi.cpp` `AfterLoad` | early-returns when `!initial_link_done` (272-274) | lazy `InitialLinkDone` when a CFI DSO loads (272-282) |
| `dlfcn.cpp` `get_libdl_info` | same logic; soname_ = linker_si.soname_ | same logic; soname_ = linker_si.soname_ |
| `linker_main.cpp` | no `android_linker_set_host_info`; `generate_tmpsoinfo` does **not** prelink (null symbol tables) | host-ELF handshake + `prelink_image` in `generate_tmpsoinfo` (744-820) |
| `linker_config.cpp` | **identical** | **identical** |

Net: the **only** behavioral difference that explains the libdl.so regression
is the `/bootstrap` skip added to `hybris/mw/libhybris`'s `linker.cpp`. The
external tree (representing closer-to-old behavior) would resolve libdl.so
from `/system/lib64/bootstrap/libdl.so` exactly as the old stock q.so did.

---

## Q4. Candidate fixes — plausibility vs. the actual source logic

### (a) Deploy a stub `/system/etc/ld.config.txt` — **NOT the fix**
A config only controls namespace isolation, search paths, permitted paths and
namespace links (`init_default_namespaces`, `linker.cpp:4359-4434`). It does
not affect the unconditional `/bootstrap` skip in `open_library_on_paths`
(`linker.cpp:1155-1161`), which runs for every path in both the env paths and
the namespace default paths. There is no libdl.so outside `/system/lib64/bootstrap`
on this device to grant access to. A config therefore cannot unblock libdl.so.
(If a *file* were made available at a non-bootstrap path — e.g. fixing the
dangling `/system/lib64/libdl.so` APEX symlink — then no config change would
even be needed, because `/system/lib64` is already in `HYBRIS_LD_LIBRARY_PATH`.)

### (b) Treat libdl.so as linker-provided — **plausible, and the cleanest fix**
The mechanism exists (`get_libdl_info`, `dlfcn.cpp:356-387`) and — thanks to
the host-ELF fix — its symbol tables are now valid. It just never matches
`"libdl.so"` by name. Minimal, source-supported ways to activate it:
   - set `__libdl_info->soname_ = "libdl.so"` at `dlfcn.cpp:378`, or
   - special-case the name in `find_loaded_library_by_soname` /
     `find_library_internal` to map `"libdl.so"` to the solinker soinfo.
Because that soinfo is `FLAG_LINKED` (`dlfcn.cpp:361`) and is already the
global symbol provider for every loaded library, returning it for
`DT_NEEDED libdl.so` mirrors real bionic's linker-provided-libdl semantics with
no new constructors run and no file I/O. This removes the dependency on any
real libdl.so file, so it is robust to the missing APEX.

### (c) Anything else the source clearly indicates
- **Remove the `/bootstrap` skip** (`linker.cpp:1155-1161`). This is the
  surgical revert to the previously-working state. Its CFI rationale is dead
  in this build (see Bottom line #3), and it is the single commit (97bd37c)
  that introduced the regression between the working old q.so and the deployed
  q.so (deployed build 19:41 is after 97bd37c 19:25; `git blame` confirms).
- Note the external tree already proves the skip is not required: it builds
  without it.

---

## Evidence trail (commands / objects)

- `git -C hybris/mw/libhybris/libhybris blame -L 1150,1162 hybris/common/q/linker.cpp`
  → skip added by commit `97bd37c` (jimmykamanga 2026-07-10 19:25).
- `git -C hybris/mw/libhybris/libhybris log -1 97bd37c` — local commit
  "guard synchronous /data/hybris-debug.log writes behind HYBRIS_DEBUG_LOG";
  carries the bootstrap-skip hunk.
- `strings hybris/mw/libhybris/libhybris/hybris/common/q/.libs/q.so | grep /bootstrap`
  → present (line 1010); q.so built 2026-07-10 19:41:56 (contains 97bd37c).
- `readelf -d hybris/mw/libhybris/libhybris/hybris/common/.libs/libhybris-common.so.1.0.0`
  → SONAME `libhybris-common.so.1` (hence synthetic libdl soname).
- `readelf -d .../common/q/.libs/q.so` → SONAME `q.so`, DT_NEEDED glibc libs
  (source of the benign `linker cannot have DT_NEEDED dependencies` warning at
  `linker.cpp:3991`).
- `diff -u external/libhybris/hybris/common/q/linker.cpp
  hybris/mw/libhybris/libhybris/hybris/common/q/linker.cpp` — only libdl-relevant
  delta is the skip.
- `diff -u` of `linker_config.cpp` → identical.

## Exact citations

- Bootstrap skip: `hybris/mw/libhybris/libhybris/hybris/common/q/linker.cpp:1155-1161`
  (commit `97bd37c`)
- "library not found": same file `linker.cpp:1501`
- `open_library` search order (LD_LIBRARY_PATH → DT_RUNPATH → default paths):
  `linker.cpp:1177-1227`
- `find_library_internal` soname-then-file flow: `linker.cpp:1596-1670`
- `find_loaded_library_by_soname`: `linker.cpp:1511-1555`
- Synthetic libdl soinfo / soname = host soname: `dlfcn.cpp:356-387` (soname at 378)
- Host-ELF population: `linker_main.cpp:744-820` (`android_linker_set_host_info`
  751-761, `generate_tmpsoinfo` 763-820); wiring at `linker_main.cpp:891-896`
- "hybris we have no libdl soinfo": `linker.cpp:4410-4425`
- CFI find_libdl: `linker_cfi.cpp:137-156`; HYBRIS_BUILD skip: `linker_cfi.cpp:230-239`
- bionic CFI neutralization: `bionic/libdl/libdl_cfi.cpp:36-46,78-82`
- Config read path: `linker.cpp:4277-4317`; read+fallback: `linker.cpp:4338-4357`;
  permissive fallback: `linker.cpp:4233-4249`; kDefaultLdPaths: `linker.cpp:154-161`
- `parse_config_file` ENOENT behavior: `linker_config.cpp:182-193`
- Debug verbosity gating (why no per-path traces at HYBRIS_LD_DEBUG=1):
  `linker_debug.h:84-98`; verbosity read at `linker_main.cpp:861-864`
