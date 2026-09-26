#!/bin/sh -e
#
# Helper script for the build process to apply entitlements

FILE="$1"
ENTITLEMENT="$2"

trap 'rm -f "$FILE.tmp"' exit
cp -pPf "$FILE" "$FILE.tmp"

if test -n "$ENTITLEMENT"; then
  codesign --entitlements "$ENTITLEMENT" --force -s - "$FILE.tmp"
fi

mv -f "$FILE.tmp" "$FILE"
trap '' exit
