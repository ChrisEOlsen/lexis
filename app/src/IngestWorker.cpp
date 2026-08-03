#include "IngestWorker.h"

extern "C" {
#include "bulk_ingest.h"
}

IngestWorker::IngestWorker(QString conninfo, qint64 corpusId, QVector<QPair<QString, QString>> newDocuments,
                            const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                            QObject *parent)
    : QThread(parent), m_conninfo(std::move(conninfo)), m_corpusId(corpusId),
      m_newDocuments(std::move(newDocuments)), m_stopwords(stopwords), m_wordnet(wordnet), m_lemmatizer(lemmatizer) {
}

void IngestWorker::run() {
    // Mirrors main.c's LEXIS_CHUNK_SIZE/LEXIS_CHUNK_OVERLAP/
    // LEXIS_INGEST_THREADS exactly -- no config UI for these yet, and
    // matching the CLI's own ingest behavior matters more right now
    // than tuning them differently here.
    constexpr size_t kChunkSize = 200;
    constexpr size_t kChunkOverlap = 40;
    constexpr int kThreadCount = 6;

    // bulk_ingest_rebuild_corpus() wants const char*const* arrays --
    // the QByteArray vectors own the UTF-8 bytes those pointers point
    // into for the duration of this call.
    QVector<QByteArray> nameBytes;
    QVector<QByteArray> textBytes;
    nameBytes.reserve(m_newDocuments.size());
    textBytes.reserve(m_newDocuments.size());
    for (const auto &doc : m_newDocuments) {
        nameBytes.append(doc.first.toUtf8());
        textBytes.append(doc.second.toUtf8());
    }

    QVector<const char *> names;
    QVector<const char *> texts;
    names.reserve(nameBytes.size());
    texts.reserve(textBytes.size());
    for (int i = 0; i < nameBytes.size(); i++) {
        names.append(nameBytes[i].constData());
        texts.append(textBytes[i].constData());
    }

    long total = bulk_ingest_rebuild_corpus(m_conninfo.toUtf8().constData(), m_corpusId, names.constData(),
                                             texts.constData(), static_cast<size_t>(names.size()), m_stopwords,
                                             m_wordnet, m_lemmatizer, kChunkSize, kChunkOverlap, kThreadCount);

    emit ingestFinished(total >= 0, total >= 0 ? static_cast<qint64>(total) : 0);
}
