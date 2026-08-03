# nss-stub

`nss_stub.c` isn't a standalone tool -- it's a small object file linked
into `tools/nano`, `tools/htop`, `tools/tmux`, and `tools/gdbserver` (and,
via the sibling `nss_stub_busybox.c` variant, `firmware_overlay/bin/busybox`
itself -- see that file's own header comment and
`firmware_overlay/etc/rc.d/rcS`'s 2026-08-04 comment for why its symbol
set is different: busybox's own `CONFIG_USE_BB_PWD_GRP` already replaces
getpwnam/getpwuid/getgrnam/getgrgid/etc, so only the real glibc NSS calls
busybox *does* still make -- getaddrinfo/gethostbyname/gethostbyaddr/
getservbyname/getservbyport, plus dlopen defensively -- need wrapping)
to work around a real, confirmed-on-hardware startup crash in statically
linked glibc binaries on this toolchain:

```
dl-call-libc-early-init.c:37: _dl_call_libc_early_init:
Assertion `sym != NULL' failed.
```

## Root cause

glibc >= 2.34 merged NSS's `dlopen()`-based service-module loading
(`libnss_files.so`, etc.) into `libc.a` itself. Even a fully static
binary that merely *references* `getpwnam`/`getpwuid`/`dlopen` (whether
or not that code path is ever actually exercised at runtime) pulls in
that static-dlopen-NSS init machinery -- which asserts and crashes at
process startup on this specific toolchain/kernel combination
(confirmed live: `nano` crashed on every invocation before this fix,
worked correctly after).

This project's first attempt at a fix -- documented (wrongly) in
`tools/nano/README.md` before this was confirmed on real hardware --
was to assume the glibc-static-linking warnings at link time
("Using 'getpwnam' in statically linked applications requires...")
were harmless. **They are not**, at least not on this ARM/glibc-2.36
combination. Static linking of NSS-dependent functions is a genuinely
unsupported combination here, not just noisy.

None of these lookups matter on this target anyway -- no
`/etc/nsswitch.conf`, a flat `/etc/passwd` only ever consulted for
uid 0, and no systemd to `dlopen`.

## Fix: `--wrap`, not just "don't call it"

Simply not calling `getpwnam` isn't enough -- the crash happens at
process startup regardless of whether the app ever exercises that code
path, because the static-dlopen-NSS init machinery gets pulled into the
binary at *link* time based on symbol references, not run time. The
fix is to make sure the *real* glibc symbols never get linked in at
all:

```sh
arm-linux-gnueabihf-gcc -c -O2 -o nss_stub.o nss_stub.c
```

Then link with `-Wl,--wrap=<symbol>` for every NSS/dlopen symbol the
target tool's build log flags (check for "Using 'X' in statically
linked applications requires..." warnings), plus `nss_stub.o` itself:

```sh
LDFLAGS="... -static -Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent /path/to/nss_stub.o"
```

`--wrap=X` redirects every call to `X` (including from inside static
libraries like `libgnu.a`/`ncurses`) to `__wrap_X` instead -- `X` itself
is never referenced, so the real implementation (and everything it
pulls in) never gets linked. `nss_stub.c` provides `__wrap_getpwnam`
etc. as trivial stubs (return `NULL`/`0`/an error as appropriate).

Add `--wrap=<symbol>` entries only for the symbols a given tool's build
log actually flags -- e.g. `gdbserver`/`htop` additionally need
`--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose` and
`--wrap=getgrgid,--wrap=getgrgid_r` (see their own build logs), while
`less` doesn't reference any of these at all and doesn't need this
treatment.

**Confirmed fixed on real hardware for `nano` (2026-07-16)** with this
exact `--wrap` treatment. One caveat worth recording: `nm <binary> |
grep -E 'dl_call_libc_early_init|nss_files_getpwnam'` still shows both
as defined (`T`) symbols even in the working, patched `nano` binary --
`_dl_call_libc_early_init` is apparently part of every static glibc
binary's crt startup on this toolchain regardless of NSS usage, and
`_nss_files_getpwnam_r` gets linked in as dead code even when nothing
calls it. Their mere presence doesn't indicate the fix failed -- the
one thing confirmed to matter is that the binary's *own* `getpwnam`/
`getpwuid`/etc. call sites resolve to `__wrap_*` (check with `nm
<binary> | grep __wrap_`), not whether every NSS-related symbol
disappears from the binary entirely. Don't use "any NSS symbol still
present" as a pass/fail signal -- it isn't one.
