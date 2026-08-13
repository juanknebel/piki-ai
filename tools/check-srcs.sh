#!/bin/sh
# Verify Makefile OBJS and tools/build-release.sh SRCS list the same sources.
# Fails if they diverge (release-only link errors).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

# Extract OBJS src list from Makefile: $(B)/foo.o -> src/foo.c
# Extract only the OBJS assignment (plus main.o used in piki rule)
objs_line=$(sed -n '/^OBJS *:=/,/^[^ \t]/p' "$ROOT/Makefile" | head -n 5 | tr '\n' ' ')
mk_srcs=$(echo "$objs_line" | grep -o '\$(B)/[a-z_]*\.o' | sed 's|.*/\([a-z_]*\)\.o|\1|' | tr '\n' ' ')
mk_list=""
for n in $mk_srcs; do mk_list="$mk_list src/$n.c"; done
if grep -q '$(B)/main.o' "$ROOT/Makefile"; then
    mk_list="src/main.c$mk_list"
fi
mk_norm=$(echo "$mk_list" | tr ' ' '\n' | grep -v '^$' | sort -u | tr '\n' ' ' | sed 's/ $//')

# Extract SRCS from build-release.sh
rel_srcs=$(sed -n 's/^SRCS=//p' "$ROOT/tools/build-release.sh" | tr -d '\"\\' | tr '\n' ' ')
# SRCS may span continuation line
rel_srcs2=$(grep -A1 '^SRCS=' "$ROOT/tools/build-release.sh" | tr -d '\"\\' | tr '\n' ' ' | sed 's/SRCS=//')
# Use the broader extraction then fallback
br_norm=$(echo "$rel_srcs2" | tr ' ' '\n' | grep -E '^src/.*\.c$' | sort | tr '\n' ' ' | sed 's/ $//')

if [ -z "$br_norm" ]; then
    br_norm=$(echo "$rel_srcs" | tr ' ' '\n' | grep -E '^src/.*\.c$' | sort | tr '\n' ' ' | sed 's/ $//')
fi

if [ "$mk_norm" != "$br_norm" ]; then
    echo "check-srcs: mismatch between Makefile and tools/build-release.sh" >&2
    echo "  Makefile : $mk_norm" >&2
    echo "  build-release.sh: $br_norm" >&2
    exit 1
fi
echo "check-srcs: ok ($mk_norm)"
