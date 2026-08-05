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
    Q_PROPERTY(CorpusListModel *corpusModel READ corpusModel CONSTANT)
    Q_PROPERTY(DocumentListModel *documentModel READ documentModel CONSTANT)
    Q_PROPERTY(ChatMessageListModel *chatModel READ chatModel CONSTANT)
    Q_PROPERTY(ChatSessionListModel *chatSessionModel READ chatSessionModel CONSTANT)
    Q_PROPERTY(qint64 activeChatSessionId READ activeChatSessionId NOTIFY activeChatSessionIdChanged)
    Q_PROPERTY(QString activeChatSessionTitle READ activeChatSessionTitle NOTIFY activeChatSessionIdChanged)
    Q_PROPERTY(bool modelReady READ isModelReady NOTIFY modelReadyChanged)
    Q_PROPERTY(bool chatBusy READ isChatBusy NOTIFY chatBusyChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    bool isConnected() const;
    qint64 activeCorpusId() const;
    QString activeCorpusName() const;
    bool isBusy() const;
    QString statusText() const;
    CorpusListModel *corpusModel() const;
    DocumentListModel *documentModel() const;
    ChatMessageListModel *chatModel() const;
    ChatSessionListModel *chatSessionModel() const;
    qint64 activeChatSessionId() const;
    QString activeChatSessionTitle() const;
    bool isModelReady() const;
    bool isChatBusy() const;

    Q_INVOKABLE bool createGroup(const QString &displayName);
    Q_INVOKABLE bool deleteGroup(qint64 corpusId);
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

signals:
    void activeCorpusIdChanged();
    void activeChatSessionIdChanged();
    void busyChanged();
    void statusTextChanged();
    void modelReadyChanged();
    void chatBusyChanged();
    // Reused for both real errors and informational results (e.g. "N
    // passages added") -- QML shows both the same way, as a dismissible
    // message dialog; splitting into two signals would just double the
    // QML-side wiring for no behavioral difference.
    void notify(QString message);

private slots:
    void onIngestFinished(bool ok, qint64 totalPassages, QStringList skipped, QStringList malformed,
                           QStringList noTextFound);
    void onModelLoadFinished(bool ok);
    void onQueryFinished(bool ok, QString answer, QVariantList sources);

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
    qint64 m_activeChatSessionId; // -1 = pending new chat, no session row created yet (see startNewChat())
    QString m_activeChatSessionTitle;
    bool m_busy;
    QString m_statusText;
    bool m_modelReady;
    bool m_chatBusy;

    StopwordSet *m_stopwords;
    WordNetTable *m_wordnet;
    Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_APPCONTROLLER_H
