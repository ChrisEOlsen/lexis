// Backs the chat panel's message list -- one row per question or
// answer, in conversation order. Never created directly from QML
// (QML_UNCREATABLE); AppController owns the one instance and exposes it
// via its chatModel property.
//
// Uses beginInsertRows()/endInsertRows() per new message, not a full
// model reset -- lets a bound QML ListView animate each new message's
// arrival individually, rather than invalidating and redrawing the
// whole list on every turn.
//
// Live answers: while a query streams, the newest assistant row is the
// "live" message -- its text grows by token, and the isLive role tells
// the delegate to render the not-yet-final prefix (through QML's
// markdownSafePrefix, the same partial-markdown guard the old
// word-by-word reveal used). beginLiveAnswer() creates the row,
// appendLiveText()/resetLiveText() grow or clear it, finishLive()
// swaps in the authoritative final text plus provenance and clears the
// flag, discardLive() removes the row entirely (a failed query leaves
// nothing behind -- same behavior as before streaming existed). All
// no-ops when no live row exists, so callers never need to track
// whether tokens ever arrived.

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
    // Which tool produced this answer: "search", "read", "chat", or empty
    // for a user message (and for legacy rows stored before the tool was
    // recorded). Not derivable from `sources`: a SEARCH that matched
    // nothing and a CHAT that retrieved nothing both have an empty list,
    // and the source inspector has to tell them apart.
    QString tool;
    // SEARCH-path retrieval provenance for the source inspector: the
    // reformulated standalone question (empty when the rewrite matched
    // the user's wording) and the space-joined term union BM25 actually
    // searched. Both empty for user messages, CHAT/SUMMARY answers, and
    // rows stored before these were recorded.
    QString searchQuery;
    QString searchTerms;
    // True only while this answer is still streaming (created by
    // beginLiveAnswer(), not yet finished). Lets the delegate render the
    // growing prefix as safely-trimmed markdown instead of the final
    // text, and hide the Source/action affordances until they exist.
    // Every other row -- user messages, finished answers, anything
    // loaded from history via setMessages() -- is false.
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

    // Replaces the whole list at once (begin/endResetModel, not per-row
    // inserts) -- for loading a chat session's full history on switch,
    // where every row changes together and there's no "new message
    // arriving" animation to preserve, unlike addMessage().
    void setMessages(const QVector<ChatMessage> &messages);

    // -- Live answer streaming (see the file comment) --
    void beginLiveAnswer();
    // Retry variant: instead of appending a new row, converts the
    // conversation's newest assistant row into the live row -- its text,
    // sources, and provenance are cleared and the streamed replacement
    // grows in place, so the improved answer replaces (rather than
    // stacks on) the one it improves, matching what QueryWorker's retry
    // does to the persisted row. A no-op when there is no assistant row
    // to convert (caller decides what that means).
    void makeLastAssistantLive();
    void appendLiveText(const QString &piece);
    void resetLiveText();
    void finishLive(const QString &text, const QVariantList &sources, const QString &tool,
                    const QString &searchQuery, const QString &searchTerms);
    // Undo of makeLastAssistantLive() for a retry that failed: puts the
    // saved original answer back into the converted row and un-lives it.
    // A no-op when no live row exists.
    void restoreLastAssistant(const QString &text, const QVariantList &sources, const QString &tool,
                              const QString &searchQuery, const QString &searchTerms);
    void discardLive();
    bool hasLiveAnswer() const { return m_liveIndex >= 0; }

private:
    // Index of the live row, -1 when no answer is streaming. Always the
    // last row while set (a live answer is by construction the newest
    // message); a plain int rather than a QPersistentModelIndex because
    // nothing else can insert or remove rows between begin and finish.
    int m_liveIndex = -1;

    QVector<ChatMessage> m_messages;
};

#endif // LEXIS_APP_CHATMESSAGELISTMODEL_H