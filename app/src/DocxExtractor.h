// Extracts plain text from a .docx file: an in-house extractor built on
// two small, permissively-licensed libraries -- libzip (the .docx
// container is just a ZIP archive) and pugixml (word/document.xml and
// its siblings are just XML) -- rather than a general-purpose document
// SDK. See ../../APP_SPEC.md's "DOCX" section for why: DocWire (the
// heavier alternative) turned out to need a build toolchain (vcpkg,
// C++20 bootstrap) disproportionate to what a narrow "get the plain
// text out" need requires, and this project already leans on small,
// Homebrew-installable dependencies elsewhere.

#ifndef LEXIS_APP_DOCXEXTRACTOR_H
#define LEXIS_APP_DOCXEXTRACTOR_H

#include <QString>

// Extracts the plain text of the .docx file at `path`: the main body
// plus any headers, footers, footnotes, and endnotes present (a
// document can have zero to several header/footer variants -- default,
// first-page, even-page -- so these are discovered by enumerating the
// archive's entries, not guessed by filename). Paragraphs are joined
// with '\n'; text within a paragraph is concatenated in document order
// across its runs. Tracked-changes deletions (<w:delText>) are a
// different element name than live text (<w:t>) and are excluded
// automatically, not via special-case logic.
//
// Returns the extracted text on success. Returns an empty (null)
// QString on failure -- the file isn't a valid zip archive, is missing
// word/document.xml, that part isn't valid XML, or no text content was
// found anywhere -- and sets *errorOut (if non-null) to a human-
// readable reason.
QString extractDocxText(const QString &path, QString *errorOut = nullptr);

#endif // LEXIS_APP_DOCXEXTRACTOR_H
