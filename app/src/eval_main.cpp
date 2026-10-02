// Batch eval harness: questions through the REAL pipeline (links QueryWorker itself, so
// runs can't drift). usage: lexis_eval <corpus_id> <questions_file> [--persist]; TSV to stdout.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVariantList>

#include "QueryWorker.h"

extern "C" {
#include "config.h"
#include "lemmatizer.h"
#include "local_llm_client.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"
}

#include <cstdio>
#include <cstdlib>

namespace {
// Mirrors AppController's constants. Conninfo is never printed (embeds the password).
const char *kStopwordsPath = "data/stopwords/english.txt";
const char *kWordnetDir = "data/wordnet";
const char *kConfigPath = "config/lexis.conf";

// Same title truncation AppController::sendChatMessage() applies.
QString titleFor(const QString &question) {
    QString title = question.trimmed();
    constexpr int kMaxTitleLength = 60;
    if (title.length() > kMaxTitleLength) {
        title = title.left(kMaxTitleLength).trimmed() + QStringLiteral("...");
    }
    return title;
}

QString flatten(QString text) {
    return text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
}
} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 3) {
        fprintf(stderr, "usage: %s <corpus_id> <questions_file> [--persist]\n", argv[0]);
        return 2;
    }
    const qint64 corpusId = args.at(1).toLongLong();
    const bool persist = args.contains(QStringLiteral("--persist"));

    QFile questionsFile(args.at(2));
    if (!questionsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fprintf(stderr, "cannot open %s\n", qPrintable(args.at(2)));
        return 1;
    }
    QStringList questions;
    QTextStream in(&questionsFile);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty()) {
            questions.append(line);
        }
    }

    char *conninfoRaw = config_load_db_conninfo(kConfigPath);
    if (conninfoRaw == nullptr) {
        fprintf(stderr, "no database configured -- set db_conninfo in %s\n", kConfigPath);
        return 1;
    }
    const QString connInfo = QString::fromUtf8(conninfoRaw);
    free(conninfoRaw);

    fprintf(stderr, "loading model...\n");
    char *modelPath = config_load_model_path(kConfigPath);
    if (modelPath == nullptr || local_llm_client_init(modelPath) != 0) {
        fprintf(stderr, "model init failed\n");
        free(modelPath);
        return 1;
    }
    free(modelPath);
    StopwordSet *stopwords = stopword_set_load(kStopwordsPath);
    WordNetTable *wordnet = wordnet_table_load(kWordnetDir);
    Lemmatizer *lemmatizer = lemmatizer_load(kWordnetDir);
    if (stopwords == nullptr || wordnet == nullptr || lemmatizer == nullptr) {
        fprintf(stderr, "language data failed to load\n");
        return 1;
    }

    PgStore *store = pg_store_open(connInfo.toUtf8().constData());
    if (store == nullptr) {
        fprintf(stderr, "cannot connect to the database\n");
        return 1;
    }

    printf("index\ttool\tpassages\tseconds\tok\tquestion\tanswer\n");
    fflush(stdout);

    QElapsedTimer wall;
    wall.start();

    for (int i = 0; i < questions.size(); i++) {
        const QString question = questions.at(i);

        // Fresh REAL session per question (the pipeline reads/writes it; a fake id would
        // fail the foreign key). Deleted after the answer unless --persist.
        const qint64 sessionId =
            pg_store_create_chat_session(store, corpusId, titleFor(question).toUtf8().constData());
        if (sessionId <= 0) {
            fprintf(stderr, "  [%d] could not create session, skipping\n", i);
            continue;
        }

        bool ok = false;
        QString answer;
        QVariantList sources;
        QString tool;

        QueryWorker worker(connInfo, corpusId, sessionId, question, stopwords, wordnet,
                            lemmatizer);
        // DirectConnection: no event loop here to pump a queued one; wait() orders it.
        QObject::connect(
            &worker, &QueryWorker::queryFinished, &app,
            [&](bool okIn, QString answerIn, QVariantList sourcesIn, QString toolIn,
                QString /*searchQueryIn*/, QString /*searchTermsIn*/) {
                ok = okIn;
                answer = answerIn;
                sources = sourcesIn;
                tool = toolIn;
            },
            Qt::DirectConnection);

        QElapsedTimer timer;
        timer.start();
        worker.start();
        worker.wait();
        const double seconds = timer.elapsed() / 1000.0;

        // Column 8: passage texts the model read, as flattened JSON (lets judges score old
        // runs without re-running the pipeline). Empty for CHAT; legacy readers ignore it.
        QJsonArray passageTexts;
        for (const QVariant &sourceVar : sources) {
            passageTexts.append(sourceVar.toMap().value(QStringLiteral("text")).toString());
        }
        const QString passagesJson =
            QString::fromUtf8(QJsonDocument(passageTexts).toJson(QJsonDocument::Compact));
        printf("%d\t%s\t%lld\t%.2f\t%d\t%s\t%s\t%s\n", i, tool.isEmpty() ? "-" : qPrintable(tool),
               static_cast<long long>(sources.size()), seconds, ok ? 1 : 0, qPrintable(flatten(question)),
               qPrintable(flatten(answer)), qPrintable(flatten(passagesJson)));
        fflush(stdout);

        if (!persist) {
            pg_store_delete_chat_session(store, sessionId);
        }

        const double elapsedMin = wall.elapsed() / 60000.0;
        const double projectedMin = (i + 1) > 0 ? elapsedMin / (i + 1) * questions.size() : 0.0;
        fprintf(stderr, "  [%d/%d] %s %.1fs  (elapsed %.1f min, projected %.1f min)\n", i + 1,
                static_cast<int>(questions.size()), qPrintable(tool), seconds, elapsedMin, projectedMin);
    }

    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    local_llm_client_cleanup();
    return 0;
}
