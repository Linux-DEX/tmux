# Welcome to tmux!

tmux is a terminal multiplexer: it enables a number of terminals to be created,
accessed, and controlled from a single screen. tmux may be detached from a
screen and continue running in the background, then later reattached.

## Installation (from source)

Requires libevent 2.x, ncurses, a C compiler, make, pkg-config, yacc or bison,
`autoconf` and `automake`:

~~~bash
git clone https://github.com/Linux-DEX/tmux.git
cd tmux
sh autogen.sh
./configure && make
sudo make install
~~~

`make install` installs to `/usr/local/bin/tmux` (`next-3.9`). It does **not**
replace a distro package at `/usr/bin/tmux`. If both exist, you can still end
up on the old binary (this fork’s borders/colors will be missing).

Remove the system package first, then install and restart any running server:

~~~bash
# Arch:            sudo pacman -R tmux
# Debian/Ubuntu:   sudo apt remove tmux
# Fedora:          sudo dnf remove tmux
# macOS (brew):    brew uninstall tmux

hash -r
tmux kill-server 2>/dev/null
tmux -V   # expect: tmux next-3.9
~~~

`/usr/local/bin` must come before `/usr/bin` in `PATH` (the usual default).
