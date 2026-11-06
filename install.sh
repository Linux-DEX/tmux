#!/usr/bin/env bash
# One-command installer for Linux-DEX/tmux (next-3.9).
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Linux-DEX/tmux/main/install.sh | bash
#   ./install.sh [--prefix DIR] [--no-sudo] [--yes] [--keep-build]

set -euo pipefail

REPO_URL="${TMUX_REPO_URL:-https://github.com/Linux-DEX/tmux.git}"
REPO_REF="${TMUX_REPO_REF:-main}"
PREFIX="${PREFIX:-/usr/local}"
ASSUME_YES=0
NO_SUDO=0
KEEP_BUILD=0
SUDO=""

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
	cat <<'EOF'
Install Linux-DEX tmux from source.

Options:
  --prefix DIR   Install prefix (default: /usr/local, or ~/.local with --no-sudo)
  --no-sudo      Install without sudo (default prefix ~/.local); package deps
                 still use sudo when needed unless already installed
  --yes          Non-interactive package installs
  --keep-build   Keep the build directory after install
  -h, --help     Show this help
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--prefix)
		[ $# -ge 2 ] || die "--prefix needs a directory"
		PREFIX=$2
		shift 2
		;;
	--prefix=*)
		PREFIX=${1#*=}
		shift
		;;
	--no-sudo)
		NO_SUDO=1
		shift
		;;
	--yes|-y)
		ASSUME_YES=1
		shift
		;;
	--keep-build)
		KEEP_BUILD=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown option: $1"
		;;
	esac
done

if [ "$NO_SUDO" -eq 1 ] && [ "$PREFIX" = /usr/local ]; then
	PREFIX="$HOME/.local"
fi

if [ "$(id -u)" -ne 0 ]; then
	if command -v sudo >/dev/null 2>&1; then
		SUDO=sudo
	elif [ "$NO_SUDO" -eq 0 ]; then
		die "sudo not found; re-run with --no-sudo --prefix \"\$HOME/.local\""
	fi
fi

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

run_root() {
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
	elif [ -n "$SUDO" ]; then
		# shellcheck disable=SC2086
		$SUDO "$@"
	else
		die "root/sudo required for: $*"
	fi
}

# make install: honor --no-sudo (user prefix must be writable)
run_install() {
	if [ "$NO_SUDO" -eq 1 ] || [ "$(id -u)" -eq 0 ]; then
		"$@"
	elif [ -n "$SUDO" ]; then
		# shellcheck disable=SC2086
		$SUDO "$@"
	else
		die "root/sudo required for: $*"
	fi
}

pkg_install() {
	# $@ = package names
	[ $# -gt 0 ] || return 0
	case "$OS" in
	macos)
		need_cmd brew
		brew install "$@"
		;;
	debian)
		if [ "$ASSUME_YES" -eq 1 ]; then
			run_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "$@"
		else
			run_root apt-get install "$@"
		fi
		;;
	fedora)
		if [ "$ASSUME_YES" -eq 1 ]; then
			run_root dnf install -y "$@"
		else
			run_root dnf install "$@"
		fi
		;;
	arch)
		if [ "$ASSUME_YES" -eq 1 ]; then
			run_root pacman -S --needed --noconfirm "$@"
		else
			run_root pacman -S --needed "$@"
		fi
		;;
	suse)
		if [ "$ASSUME_YES" -eq 1 ]; then
			run_root zypper --non-interactive install "$@"
		else
			run_root zypper install "$@"
		fi
		;;
	*)
		die "unsupported OS for dependency install: $OS"
		;;
	esac
}

detect_os() {
	case "$(uname -s)" in
	Darwin)
		OS=macos
		;;
	Linux)
		if [ -r /etc/os-release ]; then
			# shellcheck disable=SC1091
			. /etc/os-release
			case "${ID_LIKE:-} ${ID:-}" in
			*debian*|*ubuntu*|*linuxmint*|*pop*) OS=debian ;;
			*rhel*|*fedora*|*centos*|*rocky*|*alma*) OS=fedora ;;
			*arch*|*manjaro*|*endeavouros*) OS=arch ;;
			*suse*|*opensuse*) OS=suse ;;
			*)
				die "unsupported Linux distro (${ID:-unknown}); install deps manually, then re-run from a clone"
				;;
			esac
		else
			die "cannot detect Linux distro (/etc/os-release missing)"
		fi
		;;
	*)
		die "unsupported OS: $(uname -s)"
		;;
	esac
}

deps_present() {
	command -v git >/dev/null 2>&1 \
		&& command -v make >/dev/null 2>&1 \
		&& command -v pkg-config >/dev/null 2>&1 \
		&& command -v gcc >/dev/null 2>&1 \
		&& command -v autoconf >/dev/null 2>&1 \
		&& command -v automake >/dev/null 2>&1 \
		&& (command -v bison >/dev/null 2>&1 || command -v yacc >/dev/null 2>&1) \
		&& pkg-config --exists libevent 2>/dev/null \
		&& { pkg-config --exists ncursesw 2>/dev/null || pkg-config --exists ncurses 2>/dev/null; }
}

install_deps() {
	if deps_present; then
		log "build dependencies already present"
	else
		log "installing build dependencies ($OS)"
		case "$OS" in
		macos)
			pkg_install libevent ncurses utf8proc pkg-config automake autoconf bison jemalloc
			;;
		debian)
			run_root apt-get update
			pkg_install \
				build-essential pkg-config autoconf automake bison \
				libevent-dev libncurses-dev
			;;
		fedora)
			pkg_install \
				gcc make pkgconf-pkg-config autoconf automake bison \
				libevent-devel ncurses-devel
			;;
		arch)
			pkg_install base-devel libevent ncurses pkgconf bison automake autoconf
			;;
		suse)
			pkg_install \
				gcc make pkg-config autoconf automake bison \
				libevent-devel ncurses-devel
			;;
		esac
	fi

	case "$OS" in
	macos)
		# Homebrew bison is keg-only; prefer it over the ancient system bison.
		if [ -x /opt/homebrew/opt/bison/bin/bison ]; then
			export PATH="/opt/homebrew/opt/bison/bin:$PATH"
		elif [ -x /usr/local/opt/bison/bin/bison ]; then
			export PATH="/usr/local/opt/bison/bin:$PATH"
		fi
		;;
	esac

	need_cmd git
	need_cmd make
	need_cmd pkg-config
}

prepare_source() {
	if [ -f configure.ac ] && [ -f autogen.sh ] && [ -f tmux.c ]; then
		SRC_DIR=$PWD
		CLEANUP_SRC=0
		log "using existing source in $SRC_DIR"
		return
	fi

	need_cmd git
	SRC_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tmux-linux-dex.XXXXXX")
	CLEANUP_SRC=1
	log "cloning $REPO_URL ($REPO_REF) into $SRC_DIR"
	git clone --depth 1 --branch "$REPO_REF" "$REPO_URL" "$SRC_DIR"
}

configure_args() {
	CONFIGURE_ARGS=(--prefix="$PREFIX")
	case "$OS" in
	macos)
		# macOS requires an explicit choice; prefer jemalloc + utf8proc when present.
		if pkg-config --exists jemalloc 2>/dev/null; then
			CONFIGURE_ARGS+=(--enable-jemalloc)
		else
			CONFIGURE_ARGS+=(--disable-jemalloc)
			warn "jemalloc not found; configuring with --disable-jemalloc"
		fi
		if pkg-config --exists libutf8proc 2>/dev/null; then
			CONFIGURE_ARGS+=(--enable-utf8proc)
		else
			CONFIGURE_ARGS+=(--disable-utf8proc)
			warn "utf8proc not found; configuring with --disable-utf8proc"
		fi
		# Homebrew ncurses / libevent often need pkg-config path help on Intel.
		if command -v brew >/dev/null 2>&1; then
			BREW_PREFIX=$(brew --prefix)
			export PKG_CONFIG_PATH="${BREW_PREFIX}/opt/ncurses/lib/pkgconfig:${BREW_PREFIX}/opt/libevent/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
			export PATH="${BREW_PREFIX}/opt/ncurses/bin:$PATH"
		fi
		;;
	esac
}

build_and_install() {
	cd "$SRC_DIR"
	log "generating configure"
	sh autogen.sh

	configure_args
	log "configuring: ./configure ${CONFIGURE_ARGS[*]}"
	./configure "${CONFIGURE_ARGS[@]}"

	JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 2)
	log "building (jobs=$JOBS)"
	make -j"$JOBS"

	log "installing to $PREFIX"
	run_install make install

	# make leaves a linker signature. macOS kills that binary (SIGKILL,
	# "Code Signature Invalid") when it is launched from a terminal.
	if [ "$OS" = macos ]; then
		log "signing $PREFIX/bin/tmux"
		run_install codesign --force --sign - "$PREFIX/bin/tmux"
	fi
}

verify_install() {
	BIN="$PREFIX/bin/tmux"
	[ -x "$BIN" ] || die "install finished but $BIN is missing"

	VERSION=$("$BIN" -V 2>/dev/null || true)
	log "installed: $VERSION ($BIN)"

	hash -r 2>/dev/null || true
	if command -v tmux >/dev/null 2>&1; then
		ACTIVE=$(command -v tmux)
		if [ "$ACTIVE" != "$BIN" ]; then
			warn "shell resolves 'tmux' to $ACTIVE, not $BIN"
			warn "put $PREFIX/bin earlier in PATH, or remove the other package"
			case "$OS" in
			macos)  warn "example: brew uninstall tmux" ;;
			debian) warn "example: sudo apt remove tmux" ;;
			fedora) warn "example: sudo dnf remove tmux" ;;
			arch)   warn "example: sudo pacman -R tmux" ;;
			suse)   warn "example: sudo zypper remove tmux" ;;
			esac
			warn "then: hash -r && tmux kill-server 2>/dev/null; tmux -V"
		fi
	else
		warn "tmux not on PATH yet; add $PREFIX/bin to PATH"
	fi

	printf '\nDone. Start with: %s\n' "$BIN"
}

cleanup() {
	if [ "${CLEANUP_SRC:-0}" -eq 1 ] && [ "$KEEP_BUILD" -eq 0 ] && [ -n "${SRC_DIR:-}" ]; then
		rm -rf "$SRC_DIR"
	fi
}
trap cleanup EXIT

detect_os
install_deps
prepare_source
build_and_install
verify_install
