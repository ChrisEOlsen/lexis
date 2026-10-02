#!/usr/bin/env bash
# Signs/notarizes LEXIS.app with a Developer ID, then builds LEXIS-signed.dmg.
# Usage: sign_and_notarize.sh "Developer ID Application: Name (TEAMID)" /path/to/LEXIS.app
set -euo pipefail

if [ $# -ne 2 ]; then
    sed -n '2,20p' "$0"
    exit 1
fi
IDENTITY="$1"
APP="$2"
KEYCHAIN_PROFILE="lexis-notary"
OUT_DIR="$(dirname "$APP")"

echo "== 1. Sign every binary, deepest first =="
# Hardened runtime required for notarization; no entitlements needed (plain client).
find "$APP" -type f \( -perm +111 -o -name '*.dylib' -o -name '*.so' \) | while read -r bin; do
    codesign --force --options runtime --timestamp -s "$IDENTITY" "$bin"
done
codesign --force --options runtime --timestamp -s "$IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"
echo "signature verifies"

echo "== 2. Notarize =="
ZIP="$OUT_DIR/LEXIS-notarize.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$KEYCHAIN_PROFILE" --wait
rm -f "$ZIP"

echo "== 3. Staple and build the final DMG =="
xcrun stapler staple "$APP"

STAGING="$(mktemp -d)"
cp -R "$APP" "$STAGING/"
ln -s /Applications "$STAGING/Applications"
rm -f "$OUT_DIR/LEXIS-signed.dmg"
hdiutil create -volname "LEXIS" -srcfolder "$STAGING" -ov -format UDZO "$OUT_DIR/LEXIS-signed.dmg"
rm -rf "$STAGING"
codesign --force --timestamp -s "$IDENTITY" "$OUT_DIR/LEXIS-signed.dmg"

echo
echo "Done: $OUT_DIR/LEXIS-signed.dmg -- this is the file to distribute."
