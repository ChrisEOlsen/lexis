// Backs the chat panel's message list (QML_UNCREATABLE; owned by AppController).
// New messages use per-row inserts (animatable); live answers stream into one flagged row.

#ifndef LEXIS_APP_CHATMESSAGELISTMODEL_H
#define LEXIS_APP_CHATMESSAGELISTMODEL_H

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVector>

struct ChatMessage {
    QString text;
    bool isUser;
    QVariantList sources; // only ever non-empty for a non-user (answer) message
    // Which tool produced this answer ("search"/"summary"/"read"/"chat"; empty for user
    // messages and legacy rows). Not derivable from `sources`: empty lists are ambiguous.
    QString tool;
    // SEARCH provenance for the source inspector: reformulated question (empty when it
    // matched the user's wording) and the space-joined term union BM25 ran.
    QString searchQuery;
    QString searchTerms;
    // True only while this answer is still streaming. The delegate renders the growing
    // prefix as safely-trimmed markdown and hides actions until finish.
    bool isLive = false;
};

class ChatMessageListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use AppController.chatModel")

public:
    enum Roles {
        TextRole = Qt::UserRole + 1,
        IsUserRole,
        SourcesRole,
        ToolRole,
        SearchQueryRole,
        SearchTermsRole,
        IsLiveRole,
    };

    explicit ChatMessageListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void addMessage(const QString &text, bool isUser, const QVariantList &sources = QVariantList(),
                    const QString &tool = QString(), const QString &searchQuery = QString(),
                    const QString &searchTerms = QString());

    // Replaces the whole list at once (full reset) -- for loading a session's history.
    void setMessages(const QVector<ChatMessage> &messages);

    // -- Live answer streaming (see the file comment) --
    void beginLiveAnswer();
    // Retry variant: converts the newest assistant row into the live row (cleared, then
    // streamed in place) instead of appending. No-op with no assistant row to convert.
    void makeLastAssistantLive();
    void appendLiveText(const QString &piece);
    void resetLiveText();
    void finishLive(const QString &text, const QVariantList &sources, const QString &tool,
                    const QString &searchQuery, const QString &searchTerms);
    // Undo of makeLastAssistantLive(): restores the saved original. No-op without a live row.
    void restoreLastAssistant(const QString &text, const QVariantList &sources, const QString &tool,
                              const QString &searchQuery, const QString &searchTerms);
    void discardLive();
    bool hasLiveAnswer() const { return m_liveIndex >= 0; }

private:
    // Live row index, -1 when nothing streams. Always the last row while set; a plain
    // int suffices since nothing else inserts/removes between begin and finish.
    int m_liveIndex = -1;

    QVector<ChatMessage> m_messages;
};

#endif // LEXIS_APP_CHATMESSAGELISTMODEL_H