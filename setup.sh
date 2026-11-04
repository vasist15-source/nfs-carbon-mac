#!/bin/sh
# Setup kit: builds the native Mac app from YOUR OWN copy of Need for Speed: Carbon (PC, version 1.4).
# Usage: ./setup.sh "/path/to/Need for Speed - Carbon" [options]   (./setup.sh --help for the options)
set -eu
cd "$(dirname "$0")"
if [ "$(uname -m)" != arm64 ]; then echo "This port needs an Apple Silicon Mac."; exit 1; fi
if ! xcode-select -p >/dev/null 2>&1; then
    echo "The Xcode Command Line Tools are needed. Run:  xcode-select --install   then run this setup again."; exit 1
fi
# A Python 3.9 to 3.12 that runs natively on Apple Silicon (the Command Line Tools include one). An Intel-only Python
# (for example an old Homebrew in /usr/local) runs under Rosetta, and everything it starts would too. Python 3.13 and
# later have no ready-made numpy 2.0.2 (kit/tools/recomp/texture-pack-requirements.txt), so pip cannot install it there.
PY_OK='import platform, sys; sys.exit(not (3, 9) <= sys.version_info[:2] <= (3, 12) or platform.machine() != "arm64")'
PYTHON=""
for p in python3.12 python3.11 python3.10 /usr/bin/python3 python3; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c "$PY_OK" 2>/dev/null; then
        PYTHON=$p; break
    fi
done
if [ -z "$PYTHON" ]; then echo "No Apple Silicon Python 3.9 to 3.12 found (the Command Line Tools include one)."; exit 1; fi
if [ ! -f kit/tools/build.py ]; then echo "The kit folder is missing: clone the whole repository again." >&2; exit 1; fi
# Checking the game folder needs nothing from the internet: no environment, no downloads.
for a in "$@"; do
    if [ "$a" = "--check-only" ]; then
        if ! "$PYTHON" -c 'import tomllib' 2>/dev/null && [ -x .venv/bin/python ]; then PYTHON=.venv/bin/python; fi
        if "$PYTHON" -c 'import tomllib' 2>/dev/null || "$PYTHON" -c 'import tomli' 2>/dev/null; then
            exec "$PYTHON" tools/setup_kit/setup.py "$@"
        fi
    fi
done
# An environment made by an earlier run with an Intel Python, or with a Python 3.13 or later, is made again with this one.
if [ -x .venv/bin/python ] && ! .venv/bin/python -c "$PY_OK" 2>/dev/null; then
    echo "Making the Python environment again with $("$PYTHON" -V 2>&1) (an earlier run used a Python the build tools don't install on)."
    rm -rf .venv
fi
if [ ! -x .venv/bin/python ]; then "$PYTHON" -m venv .venv; fi
# Ready-made wheels only: a package with none for this Python fails at once instead of compiling for minutes.
# pip's own words go to build/setup.log (the Setup app's Show the log), which the setup starts afresh later.
mkdir -p build
if ! .venv/bin/python -m pip install --quiet --disable-pip-version-check --retries 8 --timeout 30 \
        --only-binary :all: -r kit/requirements-dev.txt tomli > build/setup.log 2>&1; then
    echo
    if grep -qiE 'NewConnectionError|ConnectTimeout|ReadTimeout|timed out|Max retries exceeded|ProxyError|SSLError|CERTIFICATE_VERIFY_FAILED|nodename nor servname|Network is unreachable|Connection (refused|reset)' build/setup.log; then
        echo "SETUP STOPPED: the setup could not download its build tools from the Python package index (pypi.org)."
        echo "Check the internet connection. A VPN, proxy or firewall app (for example Little Snitch or LuLu) may be"
        echo "blocking Python or the Setup app: allow them to connect, or pause it, and run the setup again."
    else
        echo "SETUP STOPPED: $(.venv/bin/python -V 2>&1) could not install the setup's build tools; the last lines of build/setup.log say why."
        grep 'ERROR' build/setup.log | tail -1
    fi
    exit 1
fi
exec .venv/bin/python tools/setup_kit/setup.py "$@"
