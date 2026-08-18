# Packaging

LEXIS ships as a signed, notarized disk image: `LEXIS-signed.dmg`.
The image mounts like a drive and contains `LEXIS.app` next to a
shortcut to `/Applications`; installing is dragging one onto the
other. The installed app has no dependencies -- no Homebrew, no
separate Postgres, no terminal steps.

Two scripts produce the release:

- `scripts/package_app.sh` builds the self-contained app bundle and
  an unsigned intermediate, `dist/LEXIS.dmg`.
- `scripts/sign_and_notarize.sh` signs the bundle with the Developer
  ID certificate, submits it to Apple's notarization service, staples
  the result, and produces the final `LEXIS-signed.dmg`. It runs on
  whichever Mac holds the certificate, which does not have to be the
  build machine.

## What the bundle contains

`package_app.sh` assembles everything the app needs into the bundle:

1. A Release build of the app as `LEXIS.app` (the dev build stays a
   plain binary; the bundle shape is a build option).
2. The Qt frameworks and QML components, placed by `macdeployqt`.
3. The read-only data: WordNet, the stopword list, the learned
   synonym table, and English OCR data.
4. A minimal PostgreSQL server (~40 MB). The app runs it privately;
   the user never interacts with it.
5. Every linked library, copied in with all Homebrew path references
   rewritten to point inside the bundle. The script verifies no
   Homebrew reference survives -- this check is what guarantees the
   app runs on a machine with nothing installed.

The models are not in the bundle: at 5.1 GB they are downloaded on
first launch instead, which keeps the installer at ~80 MB.

## First launch

The installed app keeps all mutable state in
`~/Library/Application Support/LEXIS/` -- the config file, the
models, and the database. The bundle itself is never written to.

On first launch the app:

1. Creates that folder and writes a default config.
2. Initializes and starts its private Postgres. The server accepts
   connections only through a file socket in a folder readable by
   that macOS user alone, and the macOS login is the credential --
   no database password exists.
3. Shows a welcome screen for the one-time model download (~5.1 GB,
   with progress; interrupted downloads resume).

Later launches reduce to instant checks. The Postgres server starts
with the app and stops with it.

Deleting the app does not delete the Application Support folder; a
reinstall finds the models and data intact. A complete removal is
both `LEXIS.app` and `~/Library/Application Support/LEXIS`.

## Signing and notarization

Gatekeeper only clears apps signed with an Apple Developer ID
certificate and notarized by Apple. `sign_and_notarize.sh` covers the
whole sequence: it signs every binary in the bundle with the hardened
runtime, submits the app for notarization (an automated scan, usually
minutes), staples Apple's ticket to the app, and packs the final DMG.
The comments at the top of the script document the one-time
credential setup on the signing machine.

The unsigned intermediate from `package_app.sh` exists only between
the two scripts. It runs normally on the build machine; on any other
Mac it hits a Gatekeeper warning, which is what the signing step
exists to remove.

## System requirements

Apple Silicon Mac. 16 GB RAM is the realistic minimum -- the chat
model alone holds ~5 GB of it. Disk: ~11 GB (app, models, and room
for the database).
