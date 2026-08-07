// Backs ChatPanel.qml's session switcher -- one row per chat session in
// the active group. Never created directly from QML (QML_UNCREATABLE);
// AppController owns the one instance and exposes it via its
// chatSessionModel property. Mirrors CorpusListModel exactly.

#ifndef LEXIS_APP_CHATSESSIONLISTMODEL_H
#define LEXIS_APP_CHATSESSIONLISTMODEL_H

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QVector>

#include "LexisEngine.h" // for the ChatSession struct

class ChatSessionListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use AppController.chatSessionModel")

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        CreatedAtRole,
    };

    explicit ChatSessionListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the whole list and notifies bound QML views. Called by
    // AppController after any operation that could change the set of
    // sessions for the active group (create, delete, group switch).
    void setSessions(const QVector<ChatSession> &sessions);

    // Linear scan (session counts per group are small -- tens, not
    // thousands) -- used by AppController to resolve activeChatSessionId
    // into a displayable title without QML having to scan roles itself.
    // Returns an empty string if sessionId isn't in the current list.
    QString titleForId(qint64 sessionId) const;

private:
    QVector<ChatSession> m_sessions;
};

#endif // LEXIS_APP_CHATSESSIONLISTMODEL_H
