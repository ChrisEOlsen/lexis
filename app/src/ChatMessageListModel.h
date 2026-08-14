// Backs the (not yet built) chat panel's message list -- one row per
// question or answer, in conversation order. Never created directly
// from QML (QML_UNCREATABLE); AppController owns the one instance and
// exposes it via its chatModel property.
//
// Uses beginInsertRows()/endInsertRows() per new message, not a full
// model reset -- lets a bound QML ListView animate each new message's
// arrival individually once the chat UI exists, rather than
// invalidating and redrawing the whole list on every turn.

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
    // true only for a message just appended live via addMessage() (this
    // process, this run); false for anything loaded from history via
    // setMessages(). Lets the message delegate tell "just arrived,
    // animate it" apart from "loaded from history, render instantly" --
    // both paths create fresh QML delegate instances either way, so
    // Component.onCompleted alone can't distinguish them; this flag is
    // the actual signal.
    bool isFresh = false;
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
        IsFreshRole,
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

private:
    QVector<ChatMessage> m_messages;
};

#endif // LEXIS_APP_CHATMESSAGELISTMODEL_H
