# Welcome to tmux!

tmux is a terminal multiplexer: it enables a number of terminals to be created,
accessed, and controlled from a single screen. tmux may be detached from a
screen and continue running in the background, then later reattached.

## Install (one command)

macOS, Debian/Ubuntu, Fedora/RHEL, Arch, and openSUSE:

```bash
curl -fsSL https://raw.githubusercontent.com/Linux-DEX/tmux/main/install.sh | bash
```

Non-root install into `~/.local`:

```bash
curl -fsSL https://raw.githubusercontent.com/Linux-DEX/tmux/main/install.sh | bash -s -- --no-sudo --yes
```

From a local clone:

```bash
./install.sh
./install.sh --prefix /usr/local --yes
```

The script installs build dependencies, builds `next-3.9`, and installs to
`/usr/local` (or `~/.local` with `--no-sudo`). Run `./install.sh --help` for
options.

## Manual install (from source)

Requires libevent 2.x, ncurses, a C compiler, make, pkg-config, yacc or bison,
`autoconf` and `automake`:

```bash
git clone https://github.com/Linux-DEX/tmux.git
cd tmux
sh autogen.sh
./configure && make
sudo make install
```

On macOS, install deps with brew first. `configure` also requires an explicit
jemalloc choice (`--enable-jemalloc` is recommended):

```bash
brew install libevent ncurses utf8proc pkg-config automake autoconf bison jemalloc
./configure --enable-jemalloc --enable-utf8proc && make
sudo make install
```

Or build without jemalloc: `./configure --disable-jemalloc --disable-utf8proc && make`

`make install` installs to `/usr/local/bin/tmux` (`next-3.9`). It does **not**
replace a distro package at `/usr/bin/tmux`. If both exist, you can still end
up on the old binary (this fork’s borders/colors will be missing).

Remove the system package first, then install and restart any running server:

```bash
# Arch:            sudo pacman -R tmux
# Debian/Ubuntu:   sudo apt remove tmux
# Fedora:          sudo dnf remove tmux
# macOS (brew):    brew uninstall tmux

hash -r
tmux kill-server 2>/dev/null
tmux -V   # expect: tmux next-3.9
```

`/usr/local/bin` must come before `/usr/bin` in `PATH` (the usual default).
