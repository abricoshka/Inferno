#!/bin/sh -e
#
# Helper script for the build process to apply entitlements

FILE="$1"
ICON="$2"
ENTITLEMENT="$3"

trap 'rm -f "$FILE.tmp"' exit
cp -pPf "$FILE" "$FILE.tmp"

if test -n "$ENTITLEMENT"; then
  codesign --entitlements "$ENTITLEMENT" --force -s - "$FILE.tmp"
fi

# Add the QEMU icon to the binary on Mac OS
Rez -append "$ICON" -o "$FILE.tmp"
SetFile -a C "$FILE.tmp"

mv -f "$FILE.tmp" "$FILE"
trap '' exit
