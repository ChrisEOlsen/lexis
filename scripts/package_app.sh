#!/usr/bin/env bash
# Builds LEXIS.app + dist/LEXIS.dmg (see dev/PACKAGE.md; signing: sign_and_notarize.sh).
# Models are NOT bundled (app downloads on first run); ad-hoc signed only.
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
# Fresh bundle: a failed run leaves Frameworks macdeployqt would half-trust.
rm -rf "$APP"
cmake -S "$ROOT/app" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DLEXIS_BUNDLE=ON >/dev/null
cmake --build "$BUILD_DIR" -j8 --target lexis_app

echo "== 2. macdeployqt =="
# qtsvg is its own keg; macdeployqt resolves its @rpath only via the app binary's rpaths.
install_name_tool -add_rpath /opt/homebrew/opt/qtsvg/lib "$APP/Contents/MacOS/LEXIS" 2>/dev/null || true
# -qmldir bundles the QML imports too, not just linked frameworks.
"$MACDEPLOYQT" "$APP" -qmldir="$ROOT/app/qml"

echo "== 3. Resources =="
mkdir -p "$RES/data" "$RES/tessdata" "$RES/pgsql/bin"
cp -R "$ROOT/data/wordnet" "$RES/data/"
cp -R "$ROOT/data/stopwords" "$RES/data/"
cp -R "$ROOT/data/synonyms" "$RES/data/"
cp "$(brew --prefix tesseract)/share/tessdata/eng.traineddata" "$RES/tessdata/"

# Minimal Postgres: bundle mirrors the Homebrew layout so relocated binaries find share/lib.
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
# -L: dereference the Homebrew opt/ symlink farms into real files.
cp -RL "$PG_SHAREDIR" "$RES/pgsql/share/$(basename "$PG_SHAREDIR")"
cp -RL "$PG_PKGLIBDIR" "$RES/pgsql/lib/$(basename "$PG_PKGLIBDIR")"
# Drop client libs/pgxs scaffolding: unneeded to run, and breaks the self-contained check.
PG_LIB="$RES/pgsql/lib/$(basename "$PG_PKGLIBDIR")"
rm -rf "$PG_LIB/pgxs"
rm -f "$PG_LIB"/libpq* "$PG_LIB"/libecpg* "$PG_LIB"/libpgtypes*

echo "== 4. Bundle non-Qt dylibs =="
# Fix what macdeployqt can't see (Postgres binaries/modules, transitive dylibs): copy to
# Frameworks, rewrite to @rpath, re-sign (install_name_tool invalidates signatures).
fix_binary() {
    local bin="$1" changed=0
    # A dylib's own install name needs -id; -change silently ignores it.
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
    # Also rewrite macdeployqt's @executable_path refs: they break non-app loaders (initdb).

    if [ "$changed" = 1 ]; then
        # One rpath relative to this binary's location (covers MacOS, pgsql tree, Frameworks).
        local rel
        rel="$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
            "$FRAMEWORKS" "$(cd "$(dirname "$bin")" && pwd -P)")"
        install_name_tool -add_rpath "@loader_path/$rel" "$bin" 2>/dev/null || true
    fi
    codesign --force -s - "$bin" >/dev/null 2>&1
}

FIX_QUEUE=()
# Includes macdeployqt's Frameworks copies (it misses transitive refs and id lines).
for bin in "$RES/pgsql/bin/"* "$APP/Contents/MacOS/LEXIS" "$FRAMEWORKS"/*.dylib; do
    fix_binary "$bin"
done
# For-loop, not `find | while`: the pipe would lose FIX_QUEUE in a subshell.
for mod in $(find "$RES/pgsql/lib" \( -name '*.so' -o -name '*.dylib' \)); do
    fix_binary "$mod"
done
# Transitive closure: copied dylibs may themselves reference /opt/homebrew.
while [ "${#FIX_QUEUE[@]}" -gt 0 ]; do
    CURRENT=("${FIX_QUEUE[@]}")
    FIX_QUEUE=()
    for bin in "${CURRENT[@]}"; do
        fix_binary "$bin"
    done
done

# Drop build-machine rpaths from the app binary (untidy in a shipped binary).
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
