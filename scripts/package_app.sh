#!/usr/bin/env bash
# Builds LEXIS.app and a distributable DMG (see dev/PACKAGE.md).
#
# What it does, in order:
#   1. Release build with -DLEXIS_BUNDLE=ON (app/build-release/LEXIS.app)
#   2. macdeployqt: Qt frameworks + QML imports into the bundle
#   3. Copies the read-only data into Contents/Resources: wordnet,
#      stopwords, the learned synonym table, tessdata (English), and a
#      minimal PostgreSQL (server + initdb/pg_ctl/createdb + share/)
#   4. Rewrites every Homebrew dylib reference so the bundle is
#      self-contained (verified: no /opt/homebrew references remain)
#   5. Ad-hoc signs the bundle and builds dist/LEXIS.dmg
#
# The result runs on this machine and any Mac that disables Gatekeeper
# checks for it; for a normal double-click install on other Macs, hand
# dist/LEXIS.dmg's app to someone with an Apple Developer ID and
# scripts/sign_and_notarize.sh.
#
# Models are NOT bundled -- the app downloads them on first run
# (~5.1GB; see SetupController). To smoke-test without downloading,
# copy existing .gguf files into
# "~/Library/Application Support/LEXIS/models/" first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/app/build-release"
DIST_DIR="$ROOT/dist"
APP="$BUILD_DIR/LEXIS.app"
RES="$APP/Contents/Resources"
FRAMEWORKS="$APP/Contents/Frameworks"

PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config
MACDEPLOYQT=/opt/homebrew/opt/qtbase/bin/macdeployqt

echo "== 1. Release build =="
# Start from a fresh bundle -- a previous (possibly failed) run leaves
# Frameworks/Resources behind that macdeployqt would half-trust.
rm -rf "$APP"
cmake -S "$ROOT/app" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DLEXIS_BUNDLE=ON >/dev/null
cmake --build "$BUILD_DIR" -j8 --target lexis_app

echo "== 2. macdeployqt =="
# qtsvg is its own keg (like qtdeclarative); the svg image-format
# plugin references QtSvg via @rpath, and macdeployqt can only resolve
# that through the app binary's rpaths -- add the keg before deploying.
install_name_tool -add_rpath /opt/homebrew/opt/qtsvg/lib "$APP/Contents/MacOS/LEXIS" 2>/dev/null || true
# -qmldir so the QML imports the app actually uses (QtQuick.Controls,
# FluentWinUI3, Layouts) get bundled too, not just linked frameworks.
"$MACDEPLOYQT" "$APP" -qmldir="$ROOT/app/qml"

echo "== 3. Resources =="
mkdir -p "$RES/data" "$RES/tessdata" "$RES/pgsql/bin"
cp -R "$ROOT/data/wordnet" "$RES/data/"
cp -R "$ROOT/data/stopwords" "$RES/data/"
cp -R "$ROOT/data/synonyms" "$RES/data/"
cp "$(brew --prefix tesseract)/share/tessdata/eng.traineddata" "$RES/tessdata/"

# Minimal PostgreSQL. The binaries locate share/ and lib/ relative to
# their own path when relocated -- but only when the runtime bin
# directory ends with the same path suffix (relative to the Homebrew
# prefix) the build compiled in: Cellar/postgresql@18/<ver>/bin next to
# share/postgresql@18 and lib/postgresql@18. So the bundle mirrors that
# exact layout under Resources/pgsql/, with a stable pgsql/bin symlink
# so the app never needs to know the version string (pg_ctl resolves
# its real path itself before computing relative paths).
PG_SHAREDIR="$($PG_CONFIG --sharedir)"   # /opt/homebrew/share/postgresql@18
PG_PKGLIBDIR="$($PG_CONFIG --pkglibdir)" # /opt/homebrew/lib/postgresql@18
PG_BINDIR="$($PG_CONFIG --bindir)"       # /opt/homebrew/Cellar/postgresql@18/<ver>/bin
PG_BIN_SUFFIX="${PG_BINDIR#/opt/homebrew/}"
rm -rf "$RES/pgsql"
mkdir -p "$RES/pgsql/$PG_BIN_SUFFIX"
for tool in postgres initdb pg_ctl createdb pg_isready; do
    cp "$PG_BINDIR/$tool" "$RES/pgsql/$PG_BIN_SUFFIX/"
done
ln -s "$PG_BIN_SUFFIX" "$RES/pgsql/bin"
mkdir -p "$RES/pgsql/share" "$RES/pgsql/lib"
# -L: the Homebrew opt/ paths are symlink farms into the Cellar --
# dereference so the bundle holds real files.
cp -RL "$PG_SHAREDIR" "$RES/pgsql/share/$(basename "$PG_SHAREDIR")"
cp -RL "$PG_PKGLIBDIR" "$RES/pgsql/lib/$(basename "$PG_PKGLIBDIR")"
# The pkglibdir tree also carries client libraries and the pgxs
# build/test scaffolding -- none of it is needed to RUN the server, and
# the test binaries would fail the self-contained check below.
PG_LIB="$RES/pgsql/lib/$(basename "$PG_PKGLIBDIR")"
rm -rf "$PG_LIB/pgxs"
rm -f "$PG_LIB"/libpq* "$PG_LIB"/libecpg* "$PG_LIB"/libpgtypes*

echo "== 4. Bundle non-Qt dylibs =="
# macdeployqt already copied and re-pointed the app binary's direct
# Homebrew dependencies (llama, pq, poppler, tesseract, ...). This pass
# does the same for everything it cannot see: the Postgres binaries,
# the server's loadable modules, and any dylib-of-a-dylib it missed.
# Every /opt/homebrew reference is copied into Contents/Frameworks and
# rewritten to @rpath; each fixed binary gets an rpath pointing there
# and a fresh ad-hoc signature (install_name_tool invalidates the old
# one, and unsigned arm64 binaries refuse to run at all).
fix_binary() {
    local bin="$1" changed=0
    # A dylib's first otool -L line is its own install name (id) --
    # -change silently ignores it, it needs -id.
    local own_id
    own_id="$(otool -D "$bin" 2>/dev/null | sed -n '2p')"
    while IFS= read -r dep; do
        local name="$(basename "$dep")"
        if [ "$dep" = "$own_id" ]; then
            install_name_tool -id "@rpath/$name" "$bin" 2>/dev/null
            changed=1
            continue
        fi
        if [ "${dep#/opt/homebrew}" != "$dep" ] && [ ! -f "$FRAMEWORKS/$name" ]; then
            cp "$dep" "$FRAMEWORKS/$name"
            chmod u+w "$FRAMEWORKS/$name"
            install_name_tool -id "@rpath/$name" "$FRAMEWORKS/$name" 2>/dev/null
            FIX_QUEUE+=("$FRAMEWORKS/$name")
        fi
        install_name_tool -change "$dep" "@rpath/$name" "$bin" 2>/dev/null
        changed=1
    done < <(otool -L "$bin" | awk 'NR>1 {print $1}' |
        grep -e '^/opt/homebrew' -e '^@executable_path/\.\./Frameworks/lib' || true)
    # The second grep pattern: macdeployqt rewrites the dylibs it copies
    # to @executable_path-style references. Those only resolve when the
    # loading EXECUTABLE is the app itself -- initdb loading the bundled
    # libpq resolved "@executable_path/../Frameworks" relative to the
    # pgsql bin directory and died. @rpath works from every loader here.

    if [ "$changed" = 1 ]; then
        # One rpath, computed relative to wherever this binary actually
        # sits -- covers Contents/MacOS, the nested pgsql tree, and the
        # Frameworks dylibs themselves uniformly.
        local rel
        rel="$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
            "$FRAMEWORKS" "$(cd "$(dirname "$bin")" && pwd -P)")"
        install_name_tool -add_rpath "@loader_path/$rel" "$bin" 2>/dev/null || true
    fi
    codesign --force -s - "$bin" >/dev/null 2>&1
}

FIX_QUEUE=()
# Includes everything macdeployqt already copied into Frameworks --
# it misses some transitive references and never rewrites id lines.
for bin in "$RES/pgsql/bin/"* "$APP/Contents/MacOS/LEXIS" "$FRAMEWORKS"/*.dylib; do
    fix_binary "$bin"
done
# A for-loop over find's output, not `find | while` -- the pipe would
# run fix_binary in a subshell and lose FIX_QUEUE. No spaces exist in
# these module names.
for mod in $(find "$RES/pgsql/lib" \( -name '*.so' -o -name '*.dylib' \)); do
    fix_binary "$mod"
done
# Transitive closure: dylibs copied above may themselves reference
# /opt/homebrew (icu -> icu, etc.).
while [ "${#FIX_QUEUE[@]}" -gt 0 ]; do
    CURRENT=("${FIX_QUEUE[@]}")
    FIX_QUEUE=()
    for bin in "${CURRENT[@]}"; do
        fix_binary "$bin"
    done
done

# Drop the build-machine rpaths still on the app binary (Homebrew keg
# paths from the dev build). Harmless at runtime -- every dependency
# entry now names a bundle path directly -- but pointless on another
# machine and untidy in a shipped binary.
otool -l "$APP/Contents/MacOS/LEXIS" | awk '/LC_RPATH/{getline; getline; print $2}' |
    grep '^/' | while read -r rp; do
        install_name_tool -delete_rpath "$rp" "$APP/Contents/MacOS/LEXIS" 2>/dev/null || true
    done
codesign --force -s - "$APP/Contents/MacOS/LEXIS" >/dev/null 2>&1

echo "== 4b. Verify self-contained =="
LEFTOVERS="$(find "$APP" -type f \( -perm +111 -o -name '*.dylib' -o -name '*.so' \) \
    -exec sh -c 'otool -L "$1" 2>/dev/null | grep -q "/opt/homebrew" && echo "$1"' _ {} \; || true)"
if [ -n "$LEFTOVERS" ]; then
    echo "ERROR: these still reference /opt/homebrew:" >&2
    echo "$LEFTOVERS" >&2
    exit 1
fi
echo "clean -- no /opt/homebrew references remain"

echo "== 5. Sign (ad hoc) and build the DMG =="
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || codesign --force -s - "$APP"

mkdir -p "$DIST_DIR"
STAGING="$(mktemp -d)"
cp -R "$APP" "$STAGING/"
ln -s /Applications "$STAGING/Applications"
rm -f "$DIST_DIR/LEXIS.dmg"
hdiutil create -volname "LEXIS" -srcfolder "$STAGING" -ov -format UDZO "$DIST_DIR/LEXIS.dmg" >/dev/null
rm -rf "$STAGING"

echo
echo "Done: $DIST_DIR/LEXIS.dmg"
du -sh "$DIST_DIR/LEXIS.dmg" "$APP"
