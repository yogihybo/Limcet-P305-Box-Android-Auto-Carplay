# tmux

Stock tmux 3.4, cross-compiled fully static for the target (ARM EABI5,
armhf). Lets a shell session survive a dropped serial/telnet
connection -- start a long-running test inside `tmux`, disconnect,
reconnect later (even from a different terminal), and reattach to the
exact same session instead of losing it.

Linked against the same static `ncurses` build used by `tools/nano`
(see `linux-arkmicro/buildroot-external/arm-static-libs/README.md`)
plus a statically-built `libevent` (`arm-static-libs/libevent-install`,
`--disable-openssl` -- no TLS needed for local pty/socket use).

## Build

```sh
tar xzf tmux-3.4.tar.gz && cd tmux-3.4
NCURSES_PREFIX=/path/to/arm-static-libs/ncurses-install
LIBEVENT_PREFIX=/path/to/arm-static-libs/libevent-install
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/tmux-install \
  --enable-static \
  CPPFLAGS="-I${NCURSES_PREFIX}/include -I${NCURSES_PREFIX}/include/ncurses -I${LIBEVENT_PREFIX}/include" \
  LDFLAGS="-L${NCURSES_PREFIX}/lib -L${LIBEVENT_PREFIX}/lib -static" \
  LIBS="-lncurses"
make -j$(nproc)
arm-linux-gnueabihf-strip -o tmux tmux
```

The `getpwnam`/`getpwuid`/`getaddrinfo`/etc glibc-static-linking
warnings at link time are expected and harmless here -- this rootfs
only does local flat-file `/etc/passwd` lookups and Unix-domain socket
communication (tmux's client/server IPC), no DNS/network lookups
involved for normal use.

## Usage

```
/ # tmux new -s work        # start a new named session
# ... run tests, detach with Ctrl-b d ...
/ # tmux attach -t work     # reattach later, from any connection
```

`tmux.conf` isn't installed on the target -- default keybindings
(`Ctrl-b` prefix) apply.
