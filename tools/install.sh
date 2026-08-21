#!/bin/sh
# Installs the latest piki release binary into $HOME/.local/bin.
#
# Usage: curl -fsSL https://raw.githubusercontent.com/juanknebel/piki-ai/master/tools/install.sh | sh
#
# Linux only. Auto-detects 32 vs 64 bit userspace and downloads the
# matching static release asset (piki-linux-x86_64 or piki-linux-i686)
# from the latest GitHub release, installing it as $HOME/.local/bin/piki.
set -eu

REPO="juanknebel/piki-ai"
DEST_DIR="$HOME/.local/bin"
DEST="$DEST_DIR/piki"

os=$(uname -s)
if [ "$os" != "Linux" ]; then
    echo "install.sh: unsupported OS '$os' (only Linux is supported)" >&2
    exit 1
fi

# 32 vs 64 bit is a userspace property, not a kernel one (a 32-bit
# userland can run on a 64-bit kernel), so probe pointer width rather
# than trusting `uname -m`.
bits=$(getconf LONG_BIT 2>/dev/null || echo 64)
case "$bits" in
    32) asset="piki-linux-i686" ;;
    64) asset="piki-linux-x86_64" ;;
    *)
        echo "install.sh: unsupported word size '$bits'" >&2
        exit 1
        ;;
esac

url="https://github.com/$REPO/releases/latest/download/$asset"

echo "install.sh: downloading $asset from the latest release..."
mkdir -p "$DEST_DIR"
tmp="$DEST.tmp.$$"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$tmp" "$url"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tmp" "$url"
else
    echo "install.sh: need curl or wget to download $url" >&2
    exit 1
fi

chmod +x "$tmp"
mv "$tmp" "$DEST"

echo "install.sh: installed $DEST"

case ":$PATH:" in
    *":$DEST_DIR:"*) ;;
    *)
        echo "install.sh: $DEST_DIR is not on your PATH -- add this to your shell profile:"
        echo "    export PATH=\"$DEST_DIR:\$PATH\""
        ;;
esac
