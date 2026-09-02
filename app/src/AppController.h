// The one object QML talks to for everything backend-related: groups,
// documents, ingestion, and chat. Wraps LexisEngine (the actual
// pg_store C-API adapter) plus the three list models QML binds its
// ListViews to. Exposed as a QML singleton -- QML creates and owns the
// single instance automatically the first time it's referenced after
// `import Lexis`, no manual registration needed in main.cpp.
//
// Owns the language data (stopwords/wordnet/lemmatizer) every ingest
// AND every chat query needs, loaded once for the app's whole lifetime
// and shared read-only across every IngestWorker/QueryWorker -- see
// IngestWorker.h's own comment on why that's safe. Also owns the local
// model's lifetime: kicks off a background ModelLoader in the
// constructor (see ModelLoader.h for why proactively, not deferred to
// first chat use) and calls local_llm_client_cleanup() in the
// destructor, after waiting for any in-flight loader/query.

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

// Full definitions, not forward declarations -- Q_PROPERTY's pointer
// types need the complete class visible for Qt's meta-type
// registration (MOC-generated code fails a static_assert otherwise).
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
    // The one group currently ingesting (-1 = none). Chat is blocked for
    // this group only -- ChatPanel swaps in an "ingestion in progress"
    // panel when it is the active group; every other group stays fully
    // usable, including chat, while the ingest runs in the background.
    Q_PROPERTY(qint64 ingestingCorpusId READ ingestingCorpusId NOTIFY ingestStateChanged)
    // 0..1 target for the progress bar. Real during extraction; when the
    // index rebuild starts (one C call, no progress hooks) it jumps to
    // near-complete and ingestAnimMs carries the estimated rebuild time,
    // so QML animates through the estimate instead of freezing.
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
    // Live pipeline progress for the chat footer: what the running query
    // is doing right now ("Searching the group...", "Reading 12
    // passages...", "Writing...", "Trying again with a deeper search...").
    // Empty when no query is running -- the footer's placeholder covers
    // that case. A new query resets it to the routing stage's text.
    Q_PROPERTY(QString queryStageText READ queryStageText NOTIFY queryStageTextChanged)
    // Settings-panel state (F5). Both apply live (no restart): thinking
    // via an explicit per-query override handed to QueryWorker (bypassing
    // generation.c's once-per-process config cache), the reranker via
    // retrieval_set_reranker_enabled(). Both persist through
    // ConfigManager's line-preserving rewrite of config/lexis.conf.
    // modelDisplayName is read-only display (its change requires a
    // restart and stays a file edit, per the spec).
    Q_PROPERTY(bool thinkingEnabled READ isThinkingEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool rerankerEnabled READ isRerankerEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString modelDisplayName READ modelDisplayName NOTIFY settingsChanged)
    // The local model's context window in tokens (LOCAL_LLM_N_CTX). CONSTANT
    // because it is a compile-time constant of the loaded model, not per
    // message -- the source inspector reports it when the READ tool fed
    // documents to the model directly, since that path's real limit is how
    // much text fits in this window.
    Q_PROPERTY(int contextTokenLimit READ contextTokenLimit CONSTANT)
    // Whether "Try harder" would actually do something right now: the
    // newest answer in THIS session came from SEARCH, its question is
    // still known, and nothing blocks a query. The button binds to this
    // instead of guessing from the message row, because the retry always
    // acts on the conversation's newest answer -- a session loaded from
    // history has answers but no live question to re-ask, and offering
    // the button there produced a click that silently did nothing.
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
    // Switches the active group and loads its documents and its chat
    // session list. Always lands on a fresh chat (see startNewChat()) --
    // it never resumes the group's most recent conversation, because a
    // question typed right after opening a group must not silently append
    // to an old thread. Resuming is explicit, via the history drawer.
    Q_INVOKABLE void selectGroup(qint64 corpusId);

    // Resets to a "pending new chat" state -- activeChatSessionId
    // becomes -1 and chatModel is cleared, but no database row is
    // created yet (see sendChatMessage()'s own comment on lazy session
    // creation). A no-op-looking call with real effect: it's what backs
    // the chat panel's "New Chat" button.
    Q_INVOKABLE void startNewChat();

    // Loads sessionId's full message history into chatModel. A no-op if
    // sessionId doesn't exist (LexisEngine::getChatMessages() just
    // returns an empty list).
    Q_INVOKABLE void selectChatSession(qint64 sessionId);

    // Permanently deletes a chat session and every message in it. If it
    // was the active session, falls back to startNewChat()'s pending
    // state rather than leaving chatModel showing a now-deleted
    // conversation.
    Q_INVOKABLE void deleteChatSession(qint64 sessionId);

    // fileUrls are raw file:// URL strings straight from QML's
    // DropArea.drop.urls -- converted to local paths here (via
    // QUrl::toLocalFile(), not string manipulation) rather than in QML,
    // since that's the robust way to handle URL-encoded characters
    // (spaces, non-ASCII filenames) correctly.
    Q_INVOKABLE void ingestFiles(const QStringList &fileUrls);

    // Requires a group to be selected and the model to be ready
    // (modelReady) -- a no-op otherwise (mirrors ingestFiles()'s own
    // guard pattern). Appends the question to chatModel immediately (so
    // it shows up right away, not after the round trip completes), then
    // the answer once QueryWorker finishes.
    Q_INVOKABLE void sendChatMessage(const QString &question);

    // System clipboard for the answer actions (QML exposes no clipboard
    // of its own; this is the one-liner a Copy button needs).
    Q_INVOKABLE void copyToClipboard(const QString &text);

    // Lets QML raise the standard message dialog (the notify() signal's
    // Main.qml handler) -- for errors a QML component detects itself,
    // like a document that can't be opened.
    Q_INVOKABLE void showMessage(const QString &message) { emit notify(message); }

    // -- Settings (F5): live + persisted via ConfigManager --
    Q_INVOKABLE void setThinkingEnabled(bool enabled);
    Q_INVOKABLE void setRerankerEnabled(bool enabled);

    // A file:// URL for the directory holding config/lexis.conf, for the
    // Settings dialog's "Open config folder" -- QML opens it with
    // Qt.openUrlExternally (Finder on macOS). The file itself is never
    // opened by the app: config edits are a deliberate human act.
    Q_INVOKABLE QString configDirectoryUrl() const;

    // Backs the canRetryLastAnswer property; retryLastAnswer() uses the
    // same call as its own guard, so the button and the action can never
    // disagree about whether a retry is possible.
    bool canRetryLastAnswer() const;

    // -- Document viewer (F1) --
    // Loads one document's stored text and chunk list for the viewer
    // dialog (see LexisEngine::getDocument()). Returns a QVariantMap
    // {"name", "text", "chunks"} (chunks: [{"chunkId", "text",
    // "tokenCount"}...]), or an empty map when the document doesn't
    // exist or the read fails -- the QML side treats empty as "cannot
    // open" and says so.
    Q_INVOKABLE QVariantMap openDocument(const QString &documentName);

    // -- Document removal (F4) --
    // Removes one document from the ACTIVE group (transactionally --
    // see pg_store_remove_document()). A no-op when a different group
    // is ingesting or this group's index is mid-rebuild (the removal
    // writes to the live schema; a rebuild's swap would race it).
    Q_INVOKABLE void removeDocument(const QString &documentName);

    // "Try harder" on the newest answer (see QueryWorker's forceRetry):
    // re-runs that question's SEARCH pipeline with the deeper retrieval
    // policy and the reasoning pass on, replacing the answer the UI
    // shows and the row history persists. A no-op when no query is
    // running, none ever ran in this session, or the newest exchange
    // wasn't a SEARCH answer (retries don't make sense for CHAT or
    // SUMMARY answers).
    Q_INVOKABLE void retryLastAnswer();

    // Starts the background model load if it never ran -- the
    // constructor skips it when the model file doesn't exist yet (fresh
    // install), and the setup overlay calls this once the download
    // finishes. A no-op while a load is in flight or already done.
    Q_INVOKABLE void retryModelLoad();

    // Aborts the running ingest (the progress panel's Cancel button).
    // Lossless: the rebuild happens in a temporary schema that only
    // replaces the group at the very end, so cancelling leaves the
    // group exactly as it was before the drop. A no-op when nothing is
    // ingesting.
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
    // Reused for both real errors and informational results (e.g. "N
    // passages added") -- QML shows both the same way, as a dismissible
    // message dialog; splitting into two signals would just double the
    // QML-side wiring for no behavioral difference.
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

private:
    void refreshCorpusModel();
    void refreshDocumentModel();

    // Re-fetches every chat session for m_activeCorpusId and repopulates
    // m_chatSessionModel -- doesn't touch activeChatSessionId or
    // chatModel, so it's safe to call after any operation that only
    // changes the *set* of sessions (delete, lazy-create) without
    // disturbing whichever session is currently open.
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
    // The question behind the running/last query, for retryLastAnswer():
    // a retry re-asks it, not a reformulation (the pipeline re-derives
    // everything else). Also remembers whether that query's answer was
    // a SEARCH answer, for the same no-op guards.
    QString m_lastQuestion;
    bool m_lastAnswerWasSearch = false;
    // The last answer's own display fields, so a failed retry can put
    // the original answer back (a "try harder" must never destroy the
    // answer it was trying to improve).
    QString m_lastAnswer;
    QVariantList m_lastAnswerSources;
    QString m_lastAnswerTool;
    QString m_lastAnswerSearchQuery;
    QString m_lastAnswerSearchTerms;
    // True while the running query is a "try harder" retry whose live row
    // is a converted (cleared) previous answer -- on failure that answer
    // is restored rather than the row discarded. See retryLastAnswer().
    bool m_retryingLiveAnswer = false;
    // Keep the m_last* block above describing the ACTIVE session's newest
    // exchange. Switching sessions without this let a retry run one
    // session's question against another session's id and overwrite an
    // unrelated answer. adoptLastExchange() reads the state back out of a
    // session loaded from history, so the button stays usable there.
    void clearLastExchange();
    void adoptLastExchange(const QVector<ChatMessage> &messages);
    QString m_modelPath; // from config/lexis.conf, resolved once in the constructor
    QString m_connInfo;  // from config/lexis.conf db_conninfo -- embeds the password, never shown

    StopwordSet *m_stopwords;
    WordNetTable *m_wordnet;
    Lemmatizer *m_lemmatizer;

    // Settings state (F5): read once at startup from the same file
    // ConfigManager writes, applied live per-query (see the property
    // comment above), and kept in sync by the setters.
    bool m_thinkingEnabled = true;
    bool m_rerankerEnabled = false;
    QString m_modelDisplayName;
};

#endif // LEXIS_APP_APPCONTROLLER_H
