// The one object QML talks to for everything backend-related (QML singleton; owns
// LexisEngine, the list models, the shared language data, and the local model lifetime).

#ifndef LEXIS_APP_APPCONTROLLER_H
#define LEXIS_APP_APPCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>

extern "C" {
#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"
}

// Full definitions: Q_PROPERTY pointer types need complete classes for MOC.
#include "ChatMessageListModel.h"
#include "ChatSessionListModel.h"
#include "CorpusListModel.h"
#include "DocumentListModel.h"

class LexisEngine;
class IngestWorker;
class ModelLoader;
class QueryWorker;

class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool connected READ isConnected CONSTANT)
    Q_PROPERTY(qint64 activeCorpusId READ activeCorpusId NOTIFY activeCorpusIdChanged)
    Q_PROPERTY(QString activeCorpusName READ activeCorpusName NOTIFY activeCorpusIdChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // Group currently ingesting (-1 = none). Chat is blocked for this group only;
    // every other group stays usable while the ingest runs in the background.
    Q_PROPERTY(qint64 ingestingCorpusId READ ingestingCorpusId NOTIFY ingestStateChanged)
    // 0..1 progress-bar target. Real during extraction; jumps to near-complete when
    // the hook-less index rebuild starts, with ingestAnimMs carrying its time estimate.
    Q_PROPERTY(double ingestProgress READ ingestProgress NOTIFY ingestStateChanged)
    Q_PROPERTY(int ingestAnimMs READ ingestAnimMs NOTIFY ingestStateChanged)
    Q_PROPERTY(QString ingestStatusText READ ingestStatusText NOTIFY ingestStateChanged)
    Q_PROPERTY(CorpusListModel *corpusModel READ corpusModel CONSTANT)
    Q_PROPERTY(DocumentListModel *documentModel READ documentModel CONSTANT)
    Q_PROPERTY(ChatMessageListModel *chatModel READ chatModel CONSTANT)
    Q_PROPERTY(ChatSessionListModel *chatSessionModel READ chatSessionModel CONSTANT)
    Q_PROPERTY(qint64 activeChatSessionId READ activeChatSessionId NOTIFY activeChatSessionIdChanged)
    Q_PROPERTY(QString activeChatSessionTitle READ activeChatSessionTitle NOTIFY activeChatSessionIdChanged)
    Q_PROPERTY(bool modelReady READ isModelReady NOTIFY modelReadyChanged)
    Q_PROPERTY(bool chatBusy READ isChatBusy NOTIFY chatBusyChanged)
    // Live pipeline stage for the chat footer ("Searching the group...", ...).
    // Empty when no query is running.
    Q_PROPERTY(QString queryStageText READ queryStageText NOTIFY queryStageTextChanged)
    // Settings state: both apply live and persist via ConfigManager. modelDisplayName
    // is read-only (changing the model is a config-file edit + restart).
    Q_PROPERTY(bool thinkingEnabled READ isThinkingEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool rerankerEnabled READ isRerankerEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString modelDisplayName READ modelDisplayName NOTIFY settingsChanged)
    // Local model's context window in tokens. CONSTANT: a property of the loaded model,
    // reported by the source inspector for retired READ-path answers.
    Q_PROPERTY(int contextTokenLimit READ contextTokenLimit CONSTANT)
    // Whether "Try harder" would do something now: newest answer in THIS session came
    // from SEARCH, its question is still known, and nothing blocks a query.
    Q_PROPERTY(bool canRetryLastAnswer READ canRetryLastAnswer NOTIFY canRetryLastAnswerChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    bool isConnected() const;
    qint64 activeCorpusId() const;
    QString activeCorpusName() const;
    bool isBusy() const;
    QString statusText() const;
    qint64 ingestingCorpusId() const { return m_ingestingCorpusId; }
    double ingestProgress() const { return m_ingestProgress; }
    int ingestAnimMs() const { return m_ingestAnimMs; }
    QString ingestStatusText() const { return m_ingestStatusText; }
    CorpusListModel *corpusModel() const;
    DocumentListModel *documentModel() const;
    ChatMessageListModel *chatModel() const;
    ChatSessionListModel *chatSessionModel() const;
    qint64 activeChatSessionId() const;
    QString activeChatSessionTitle() const;
    bool isModelReady() const;
    bool isChatBusy() const;
    QString queryStageText() const { return m_queryStageText; }
    bool isThinkingEnabled() const { return m_thinkingEnabled; }
    bool isRerankerEnabled() const { return m_rerankerEnabled; }
    QString modelDisplayName() const { return m_modelDisplayName; }
    int contextTokenLimit() const;

    Q_INVOKABLE bool createGroup(const QString &displayName);
    Q_INVOKABLE bool deleteGroup(qint64 corpusId);
    // Switches the active group. Always lands on a fresh chat, never the most recent
    // conversation; resuming is explicit, via the history drawer.
    Q_INVOKABLE void selectGroup(qint64 corpusId);

    // Resets to "pending new chat" (id -1, cleared model); the session row is created
    // lazily by sendChatMessage(). Backs the "New Chat" button.
    Q_INVOKABLE void startNewChat();

    // Loads sessionId's full history into chatModel. No-op if sessionId doesn't exist.
    Q_INVOKABLE void selectChatSession(qint64 sessionId);

    // Deletes a chat session and its messages. Falls back to startNewChat() if active.
    Q_INVOKABLE void deleteChatSession(qint64 sessionId);

    // fileUrls are raw file:// strings from DropArea.drop.urls, converted to local paths
    // here via QUrl::toLocalFile() (handles URL-encoded characters correctly).
    Q_INVOKABLE void ingestFiles(const QStringList &fileUrls);

    // No-op unless a group is selected and the model is ready. Shows the question
    // immediately, then the answer once QueryWorker finishes.
    Q_INVOKABLE void sendChatMessage(const QString &question);

    // System clipboard for the Copy buttons (QML exposes no clipboard of its own).
    Q_INVOKABLE void copyToClipboard(const QString &text);

    // Lets QML raise the standard message dialog for errors it detects itself.
    Q_INVOKABLE void showMessage(const QString &message) { emit notify(message); }

    // -- Settings (F5): live + persisted via ConfigManager --
    Q_INVOKABLE void setThinkingEnabled(bool enabled);
    Q_INVOKABLE void setRerankerEnabled(bool enabled);

    // file:// URL of the config directory, for Settings' "Open config folder".
    Q_INVOKABLE QString configDirectoryUrl() const;

    // Backs canRetryLastAnswer; retryLastAnswer() uses it as its guard too.
    bool canRetryLastAnswer() const;

    // Document viewer payload: {"name", "text", "chunks"}, or an empty map when the
    // document can't be read (QML reports that as "cannot open").
    Q_INVOKABLE QVariantMap openDocument(const QString &documentName);

    // Removes one document from the ACTIVE group. No-op while its index is mid-rebuild
    // (the removal writes the live schema; a rebuild's swap would race it).
    Q_INVOKABLE void removeDocument(const QString &documentName);

    // "Try harder": re-runs the newest answer's SEARCH pipeline with deeper retrieval
    // and the reasoning pass on, replacing the answer in the UI and in history.
    Q_INVOKABLE void retryLastAnswer();

    // Starts the background model load if it never ran (fresh installs skip it until the
    // setup download lands). No-op while a load is in flight or already done.
    Q_INVOKABLE void retryModelLoad();

    // Aborts the running ingest. Lossless (rebuild runs in a temp schema); no-op when
    // nothing is ingesting.
    Q_INVOKABLE void cancelIngest();

signals:
    void activeCorpusIdChanged();
    void activeChatSessionIdChanged();
    void busyChanged();
    void statusTextChanged();
    void ingestStateChanged();
    void modelReadyChanged();
    void chatBusyChanged();
    void queryStageTextChanged();
    void canRetryLastAnswerChanged();
    // Thinking/reranker/model-display changed (Settings panel).
    void settingsChanged();
    // One signal for errors and informational results alike; QML shows both the same way.
    void notify(QString message);

private slots:
    void onIngestFinished(bool ok, bool cancelled, qint64 totalPassages, QStringList skipped,
                            QStringList malformed, QStringList noTextFound);
    void onIngestProgress(int filesDone, int filesTotal, qint64 indexEtaMs);
    void onModelLoadFinished(bool ok);
    void onQueryFinished(bool ok, QString answer, QVariantList sources, QString tool,
                          QString searchQuery, QString searchTerms);
    void onQueryStage(int stage, int payload);
    void onQueryToken(QString piece);
    // Fired before onQueryFinished(false) on model failure; reason replaces the generic notice.
    void onQueryFailed(QString reason);

private:
    void refreshCorpusModel();
    void refreshDocumentModel();

    // Re-fetches this group's sessions. Leaves the open session and chatModel alone.
    void refreshChatSessionModel();

    std::unique_ptr<LexisEngine> m_engine;
    CorpusListModel *m_corpusModel;
    DocumentListModel *m_documentModel;
    ChatMessageListModel *m_chatModel;
    ChatSessionListModel *m_chatSessionModel;
    IngestWorker *m_activeWorker;    // non-owning; deletes itself via QThread::finished -> deleteLater()
    ModelLoader *m_modelLoader;      // non-owning; deletes itself the same way, cleared once modelReady
    QueryWorker *m_activeQueryWorker; // non-owning; same self-deletion pattern

    qint64 m_activeCorpusId;
    QString m_activeCorpusName;
    qint64 m_ingestingCorpusId = -1;
    double m_ingestProgress = 0.0;
    int m_ingestAnimMs = 0;
    QString m_ingestStatusText;
    qint64 m_activeChatSessionId; // -1 = pending new chat, no session row created yet (see startNewChat())
    QString m_activeChatSessionTitle;
    bool m_busy;
    QString m_statusText;
    bool m_modelReady;
    bool m_chatBusy;
    QString m_queryStageText;
    // Question behind the running/last query; a retry re-asks it verbatim.
    QString m_lastQuestion;
    bool m_lastAnswerWasSearch = false;
    // Last answer's display fields, so a failed retry can restore the original.
    QString m_lastAnswer;
    QVariantList m_lastAnswerSources;
    QString m_lastAnswerTool;
    QString m_lastAnswerSearchQuery;
    QString m_lastAnswerSearchTerms;
    // True while a retry is streaming into a converted previous answer; on failure that
    // answer is restored rather than the row discarded.
    bool m_retryingLiveAnswer = false;
    // Set by onQueryFailed, consumed once by the matching onQueryFinished(false).
    QString m_queryFailureReason;
    // Keep m_last* describing the ACTIVE session's newest exchange; adoptLastExchange()
    // re-derives it from history so the retry button stays usable there.
    void clearLastExchange();
    void adoptLastExchange(const QVector<ChatMessage> &messages);
    QString m_modelPath; // from config/lexis.conf, resolved once in the constructor
    QString m_connInfo;  // from config/lexis.conf db_conninfo -- embeds the password, never shown

    StopwordSet *m_stopwords;
    WordNetTable *m_wordnet;
    Lemmatizer *m_lemmatizer;

    // Settings state: read at startup, applied live per-query, kept in sync by setters.
    bool m_thinkingEnabled = true;
    bool m_rerankerEnabled = false;
    QString m_modelDisplayName;
};

#endif // LEXIS_APP_APPCONTROLLER_H
