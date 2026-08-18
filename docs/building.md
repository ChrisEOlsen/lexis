# Building and running

macOS, Apple Silicon. Everything installs through Homebrew.

## Dependencies

```
brew install postgresql@18 llama.cpp ggml cmake pkgconf \
             qtbase qtdeclarative poppler pugixml tesseract leptonica libzip
```

Note: the Makefile and `app/CMakeLists.txt` pin the exact Homebrew
Cellar paths for llama.cpp and ggml (Homebrew doesn't give them stable
version-free paths). If your installed versions differ, update
`LLAMA_CPP_DIR` and `GGML_DIR` in both files and the include paths in
`compile_flags.txt`.

## Database

PostgreSQL runs locally on port 5434 (chosen to avoid colliding with
any existing Postgres on 5432):

```
# one-time: set the port
sed -i '' 's/^#port = 5432/port = 5434/' /opt/homebrew/var/postgresql@18/postgresql.conf

make pg-start
# choose your own password here...
/opt/homebrew/opt/postgresql@18/bin/psql -p 5434 -d postgres \
  -c "CREATE ROLE lexis LOGIN PASSWORD 'your-password-here';"
/opt/homebrew/opt/postgresql@18/bin/createdb -p 5434 -O lexis lexis
/opt/homebrew/opt/postgresql@18/bin/createdb -p 5434 -O lexis lexis_test
```

...and put the same password in `config/lexis.conf`'s `db_conninfo`
line (see the next step). That file stays on your machine -- it is
never committed. If the test suite should use a different connection,
set the `LEXIS_TEST_CONNINFO` environment variable.

`make pg-stop` shuts it down. Tables are created automatically on
first use.

## Models

```
cp config/lexis.conf.example config/lexis.conf
./scripts/download_model.sh    # chat model, ~5GB
curl -L -o data/models/bge-small-en-v1.5-f16.gguf \
  "https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-f16.gguf"
```

## Build

```
make lexis                 # the CLI
make check                 # the test suite (needs the database running)

cmake -S app -B app/build  # the desktop app
cmake --build app/build -j8
./app/build/lexis_app
```

Run everything from the project root -- data files are found by
relative path.

## First run

Open the app, click "+ New Group", drop a few documents in, and ask a
question. The first question after launch waits a few extra seconds
while the models load.

## Packaging the macOS app

`scripts/package_app.sh` builds a self-contained LEXIS.app and a DMG
in `dist/`. The bundle carries the Qt frameworks, every library the
app links, the language data, English OCR data, and a private
PostgreSQL server -- nothing from Homebrew is needed on the machine
that runs it.

The installed app keeps its own state in
`~/Library/Application Support/LEXIS/`: the config file, the
downloaded models, and the database. Its Postgres listens on a unix
socket in a private directory instead of a network port, and the
macOS user account is the login -- there is no database password.
On first launch the app offers to download the two models (about
5.1 GB); everything else works out of the box.

The DMG out of `package_app.sh` is unsigned, so other Macs will warn
before opening it. `scripts/sign_and_notarize.sh` signs and notarizes
the app -- it must run on a Mac with an Apple Developer ID
certificate; the comments at the top of the script walk through the
one-time setup.
