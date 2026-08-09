// Batch evaluation harness: runs a list of questions through the REAL chat
// pipeline and records what came back.
//
// This links the app's own QueryWorker rather than reimplementing the
// pipeline, which is the entire point -- every earlier measurement in this
// project was made by a scratch program that mirrored QueryWorker by hand,
// and a mirror can drift from what the UI actually does. Here the routing,
// reformulation, term union, retrieval, trimming, generation and provenance
// are literally the same code the app runs; only the Qt UI and the
// AppController state machine are absent.
//
// Each question gets its own fresh chat session, mirroring what the app does
// when you open a group and ask something: selectGroup() always lands on a
// new chat, so there is no conversation history unless the user builds it.
// That isolates each question -- history effects are real (measured earlier
// in this project) but they are a separate experiment.
//
// usage: lexis_eval <corpus_id> <questions_file> [--persist]
//
//   --persist  write each exchange to public.chat_sessions/chat_messages so
//              the run can be browsed in the app afterwards. Off by default:
//              a few hundred sessions makes the history drawer unusable.
//
// Output: TSV to stdout -- index, tool, passage_count, seconds, ok, question,
// answer (tabs and newlines flattened so one exchange stays one row).

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVariantList>

#include "QueryWorker.h"

extern "C" {
#include "lemmatizer.h"
#include "local_llm_client.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"
}

#include <cstdio>

namespace {
// Mirrors AppController's own constants -- see its kConnInfo comment on why
// the conninfo is never printed.
const char *kConnInfo = "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only";
const char *kStopwordsPath = "data/stopwords/english.txt";
const char *kWordnetDir = "data/wordnet";
const char *kModelPath = "data/models/gemma-4-E2B-it-Q4_K_M.gguf";

// Same truncation AppController::sendChatMessage() applies when it titles a
// new chat from its first question.
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

    fprintf(stderr, "loading model...\n");
    if (local_llm_client_init(kModelPath) != 0) {
        fprintf(stderr, "model init failed\n");
        return 1;
    }
    StopwordSet *stopwords = stopword_set_load(kStopwordsPath);
    WordNetTable *wordnet = wordnet_table_load(kWordnetDir);
    Lemmatizer *lemmatizer = lemmatizer_load(kWordnetDir);
    if (stopwords == nullptr || wordnet == nullptr || lemmatizer == nullptr) {
        fprintf(stderr, "language data failed to load\n");
        return 1;
    }

    PgStore *store = pg_store_open(kConnInfo);
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

        // A fresh session per question, exactly as the app does on a new
        // chat. Always a REAL session, even when not persisting: the
        // pipeline reads history from it and writes both messages to it, so
        // a fake id would make every one of those calls fail against the
        // foreign key and stop this being a faithful run. When persistence
        // is off the session is deleted after the answer instead, which
        // cascades its messages away.
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

        QueryWorker worker(QString::fromUtf8(kConnInfo), corpusId, sessionId, question, stopwords, wordnet,
                            lemmatizer);
        // DirectConnection: the lambda runs on the worker thread. There is no
        // event loop here to pump a queued connection, and wait() below makes
        // the ordering safe.
        QObject::connect(
            &worker, &QueryWorker::queryFinished, &app,
            [&](bool okIn, QString answerIn, QVariantList sourcesIn, QString toolIn) {
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

        printf("%d\t%s\t%lld\t%.2f\t%d\t%s\t%s\n", i, tool.isEmpty() ? "-" : qPrintable(tool),
               static_cast<long long>(sources.size()), seconds, ok ? 1 : 0, qPrintable(flatten(question)),
               qPrintable(flatten(answer)));
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
