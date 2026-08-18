# Packaging: building the installer

How to turn the source tree into `LEXIS.dmg` -- the one-file installer
a user opens, drags to Applications, and runs. No Homebrew, no
terminal, no Postgres setup on their side.

## What a DMG is

A `.dmg` is a macOS disk image: one file that mounts like a drive when
double-clicked. Ours contains `LEXIS.app` and a shortcut to
`/Applications`, so installing is dragging one icon onto the other.
That is the entire install.

## Building it

One command, from the project root, on a machine already set up for
development (the same Homebrew packages `docs/building.md` lists):

```bash
./scripts/package_app.sh
```

Takes a few minutes. The result is `dist/LEXIS.dmg` (~80 MB). The
script stops with a clear error if any step fails.

What it does, step by step:

1. **Release build.** Compiles the app into a `LEXIS.app` bundle
   (`app/build-release/`) instead of the plain binary the dev build
   makes.
2. **Qt bundling.** Runs `macdeployqt`, Qt's own tool that copies the
   Qt frameworks and QML files the app uses into the bundle.
3. **Data files.** Copies WordNet, the stopword list, the learned
   synonym table, and English OCR data into the bundle's Resources.
4. **A private PostgreSQL.** Copies a minimal Postgres server (~40 MB)
   into the bundle. The installed app runs it itself; the user never
   knows it is there.
5. **Library rewiring.** Every library the app or Postgres needs is
   copied into the bundle and every reference to a Homebrew path is
   rewritten to point inside the bundle. The script then verifies not
   a single Homebrew reference remains -- this is what makes the app
   work on a Mac with nothing installed.
6. **DMG.** Signs the app with a placeholder signature and packs it
   into `dist/LEXIS.dmg`.

The models are NOT in the DMG -- they are 5.1 GB. The app downloads
them on first launch instead (see below).

## What happens on the user's first launch

1. The app sets up its home folder:
   `~/Library/Application Support/LEXIS/` -- config file, models,
   database all live there. The app bundle itself is never written to.
2. It initializes and starts its private Postgres. The database
   accepts connections only through a file socket in a folder only
   that macOS user can read, and the macOS login is the credential --
   there is no database password anywhere.
3. A welcome screen offers the one-time model download (~5.1 GB, with
   a progress bar; safe to interrupt, it resumes).
4. The normal app. On later launches, steps 1-3 are instant checks.

Deleting the app does not delete that home folder -- a reinstall
finds the models and data again. Removing LEXIS completely means
deleting both `LEXIS.app` and `~/Library/Application Support/LEXIS`.

## Testing the DMG on the build machine

Open `dist/LEXIS.dmg`, drag LEXIS to Applications, launch it. To see
the full first-run experience (welcome screen and download), make sure
`~/Library/Application Support/LEXIS/models/` is empty first --
otherwise the app finds the models and skips setup.

## Signing: why other Macs complain, and the fix

macOS only trusts apps signed with an Apple Developer ID ($99/year
account). Our DMG is not, so on any other Mac, Gatekeeper warns and
the user has to right-click > Open to get past it.

The fix is `scripts/sign_and_notarize.sh`, run by someone who has a
Developer ID certificate on their Mac (it does not have to be the
build machine):

```bash
./scripts/sign_and_notarize.sh "Developer ID Application: Their Name (TEAMID)" /path/to/LEXIS.app
```

It signs every binary in the bundle, uploads the app to Apple's
notarization service (an automated malware scan, usually minutes),
staples Apple's approval to the app, and produces `LEXIS-signed.dmg`.
That file installs on any Mac with no warnings. The comments at the
top of the script cover the one-time credential setup.

## Requirements for the installed app

Apple Silicon Mac, 16 GB RAM realistic minimum (the chat model alone
needs ~5 GB of it), ~11 GB free disk (app + models + room for the
database).
