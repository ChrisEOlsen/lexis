---
tags: [app, ingestion]
---

# Document Extractors

**Format adapters: file → plain text** for the app's drag-and-drop ingestion.

Source: `app/src/PdfExtractor.cpp`, `DocxExtractor.cpp`, `OcrExtractor.cpp`.

- **PDF** — poppler-cpp.
- **DOCX** — in-house extractor over pugixml + libzip (a .docx is a zip of XML).
- **Images/scans** — tesseract OCR (+ leptonica for image loading); tessdata path baked in at build time from `brew --prefix tesseract`.

All four libraries arrive via pkg-config as real IMPORTED targets — unlike the hand-pathed postgresql/llama.cpp/ggml dependencies (see `app/CMakeLists.txt`).

**Used by:** [[Ingest Worker]].
