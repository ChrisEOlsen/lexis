// In-house .docx text extraction on libzip + pugixml (see dev/APP_SPEC.md's "DOCX"
// section for why, not a general document SDK).

#ifndef LEXIS_APP_DOCXEXTRACTOR_H
#define LEXIS_APP_DOCXEXTRACTOR_H

#include <QString>

// Body plus any headers/footers/footnotes/endnotes, '\n'-joined per paragraph.
// Empty QString + *errorOut reason on failure (bad archive, missing/invalid XML, no text).
QString extractDocxText(const QString &path, QString *errorOut = nullptr);

#endif // LEXIS_APP_DOCXEXTRACTOR_H
