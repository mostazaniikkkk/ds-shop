#!/bin/sh
# Builds the homebrew with devkitPro.
# make does not support paths with spaces, so it builds through a
# virtual drive (subst) pointing to this folder.
cd "$(dirname "$0")"
HERE="$(cygpath -w "$(pwd)")"
DRIVE=""
for L in Q R S T U V W; do
	T="$(subst 2>/dev/null | grep -i "^$L:" | sed 's/^.*=> //')"
	if [ "$T" = "$HERE" ]; then DRIVE=$L; break; fi
	if [ -z "$T" ] && [ ! -d "/$(echo $L | tr A-Z a-z)" ]; then
		subst "$L:" "$HERE" && DRIVE=$L && break
	fi
done
[ -n "$DRIVE" ] || { echo "No hay letra de unidad libre para subst"; exit 1; }
D=$(echo $DRIVE | tr A-Z a-z)
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
exec /c/devkitPro/msys2/usr/bin/bash.exe -lc "cd /$D/ && make $*"
