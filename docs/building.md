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
/opt/homebrew/opt/postgresql@18/bin/psql -p 5434 -d postgres \
  -c "CREATE ROLE lexis LOGIN PASSWORD 'lexis_dev_only';"
/opt/homebrew/opt/postgresql@18/bin/createdb -p 5434 -O lexis lexis
/opt/homebrew/opt/postgresql@18/bin/createdb -p 5434 -O lexis lexis_test
```

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
