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
