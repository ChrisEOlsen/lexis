#!/usr/bin/env bash
# Signs and notarizes LEXIS.app with a real Apple Developer ID, then
# builds the final DMG. Run this on a Mac that has the Developer ID
# certificate in its keychain -- it is meant for the person with the
# $99/year Apple Developer account, not for the build machine.
#
# One-time setup on that Mac:
#   1. Install the "Developer ID Application" certificate (Xcode ->
#      Settings -> Accounts, or developer.apple.com).
#   2. Store notary credentials once:
#        xcrun notarytool store-credentials lexis-notary \
#          --apple-id you@example.com --team-id TEAMID \
#          --password <app-specific password from appleid.apple.com>
#
# Usage:
#   ./sign_and_notarize.sh "Developer ID Application: Name (TEAMID)" /path/to/LEXIS.app
#
# Input: the UNSIGNED (ad-hoc) LEXIS.app out of package_app.sh's DMG --
# mount the DMG and drag the app somewhere writable first.
# Output: LEXIS-signed.dmg next to the app, notarized and stapled, safe
# to hand to anyone.
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
# Hardened runtime is required for notarization. No entitlements are
# needed: the app only downloads models (plain network client, not
# restricted by hardened runtime) and touches user-directory files.
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
