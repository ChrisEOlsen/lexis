# LEXIS Launch Plan

**Goal:** Anyone can visit the website, download LEXIS for macOS, install it,
and use it. Nothing else.

**What we're shipping:** LEXIS is a macOS (Apple Silicon) desktop app that lets
you chat with very large documents completely offline. Lexical search plus
local models. No API keys, no cloud, no per-question cost. The repo already
has a full packaging pipeline: `scripts/package_app.sh` builds the app bundle
and `scripts/sign_and_notarize.sh` produces a signed, notarized
`LEXIS-signed.dmg` (~80 MB). On first launch the app downloads ~5.1 GB of
models with a progress bar and resume support.

---

## Phase 1 — Build the release

On the Mac that holds the Developer ID certificate:

1. `git pull` the lexis repo, clean state, on the release commit.
2. `./scripts/package_app.sh` — produces unsigned `dist/LEXIS.dmg`.
3. `./scripts/sign_and_notarize.sh` — signs with the hardened runtime,
   submits to Apple's notarization service, staples the ticket, produces
   `LEXIS-signed.dmg`.
   - One-time setup (certificates, notarytool credentials) is documented in
     the comments at the top of `sign_and_notarize.sh`. Chris has an Apple
     Developer account, so this is just the one-time keychain/API key setup.
4. Sanity check the DMG mounts and the app launches on the build machine.

**Done when:** `LEXIS-signed.dmg` exists and opens without a Gatekeeper
warning on the build Mac.

## Phase 2 — Hosting (all Cloudflare)

- **R2 bucket** for the installer (e.g. `lexis-downloads`). ~80 MB file, and
  R2 has no egress fees, so downloads cost nothing. Upload
  `LEXIS-signed.dmg` and make it publicly reachable (custom domain or the
  bucket's public URL).
- **Cloudflare Pages** for the landing page. Static site, free, deploys from
  GitHub. Put the site in a `site/` folder in the lexis repo (or a separate
  repo) and point Pages at it.
- **Domain:** Chris picks one. Add it to Cloudflare, point DNS at the Pages
  site. Use a `downloads.` subdomain or the R2 custom domain for the DMG
  link so the file URL stays stable across releases.

**Done when:** `https://<domain>/` serves the landing page and the download
button resolves to the DMG in R2.

## Phase 3 — Landing page

One page, one job: convince and convert. Contents:

1. **Hero:** what it is in one line — "Chat with very large documents,
   completely offline." Download for macOS button, front and center.
2. **How it works (3 steps):** Download → drag to Applications → drop
   documents in and ask questions.
3. **Why it's different:** lexical search you can inspect (visible search
   terms and source passages) vs. black-box vector RAG; indexing is minutes
   not hours; no API keys, no cloud, no per-question cost.
4. **System requirements, stated plainly:** Apple Silicon Mac, 16 GB RAM
   recommended, ~11 GB disk (app + models + database room). macOS only —
   say so, don't bury it.
5. **FAQ:** Is my data sent anywhere? (No — everything runs on your Mac.)
   What does it cost? (whatever Chris decides — see open decisions.)
   What happens on first launch? (One-time ~5 GB model download, resumable.)
6. **Footer:** privacy policy link, contact/support link.

Keep it static HTML/CSS. No framework needed. Match the app's tone: plain
and direct.

**Done when:** The page answers "what is it, will it run on my Mac, and how
do I get it" within 30 seconds.

## Phase 4 — Legal minimum

The app itself is fully offline — no accounts, no telemetry, no data leaves
the machine. That makes this light:

1. **Privacy policy page** on the site. One page, plain language: the app
   collects nothing and transmits nothing; the website collects
   [nothing | Cloudflare Web Analytics — Chris decides].
2. **Terms of use** — short: the software is provided as-is, don't
   redistribute the installer, etc.

**Done when:** Both pages are live and linked from the landing page footer.

## Phase 5 — Test like a stranger

On a Mac that has never seen the repo (fresh user account or second Mac):

1. Go to the site, click download.
2. Open the DMG — no Gatekeeper warning should appear (this is what the
   signing/notarization step buys).
3. Drag LEXIS.app to Applications, launch it.
4. First-launch flow: welcome screen, ~5 GB model download with progress,
   interrupt and resume it once to confirm resume works.
5. Drop in a real document, ask a real question, confirm the answer and
   its cited sources.

**Done when:** A cold install works end to end with zero terminal steps.

## Phase 6 — Launch checklist

- [ ] `LEXIS-signed.dmg` built, signed, notarized
- [ ] DMG uploaded to R2, public URL tested with `curl`
- [ ] Landing page live on the domain via Cloudflare Pages
- [ ] Download button tested from a phone (proves the link works off-network)
- [ ] Privacy policy and terms pages live and linked
- [ ] Cold-install test passed (Phase 5)
- [ ] Support contact works (whoever replies to user email/issues)

---

## Post-launch (not blocking v1)

- **Auto-updates:** add Sparkle so the app can update itself instead of
  users re-downloading DMGs.
- **Versioned releases:** tag releases in git (`v1.0.0`), keep old DMGs in
  R2, keep a stable "latest" download URL.
- **Feedback channel:** GitHub Issues on the lexis repo, or a support email.
- **Windows/Linux:** only if demand appears. macOS-only is the whole point
  of this launch.

## Open decisions (Chris)

1. **Domain** for the landing page.
2. **Price:** free, paid, or free now / paid later? (The bundle currently
   has no licensing — free is the zero-work option for v1.)
3. **Website analytics:** none, or Cloudflare Web Analytics (privacy-friendly).
4. **Support channel:** email address or GitHub Issues.
5. **Site location:** `site/` folder in the lexis repo vs. a separate repo.
