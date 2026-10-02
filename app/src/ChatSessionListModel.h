// One row per chat session in the active group (QML_UNCREATABLE; owned by
// AppController). Mirrors CorpusListModel.

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

    // Replaces the whole list. Called after create, delete, or group switch.
    void setSessions(const QVector<ChatSession> &sessions);

    // Linear scan (session counts are small). Empty string if sessionId isn't listed.
    QString titleForId(qint64 sessionId) const;

private:
    QVector<ChatSession> m_sessions;
};

#endif // LEXIS_APP_CHATSESSIONLISTMODEL_H
