#include "ChatSessionListModel.h"

ChatSessionListModel::ChatSessionListModel(QObject *parent) : QAbstractListModel(parent) {
}

int ChatSessionListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_sessions.size();
}

QVariant ChatSessionListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sessions.size()) {
        return QVariant();
    }
    const ChatSession &session = m_sessions.at(index.row());
    switch (role) {
    case IdRole:
        return session.id;
    case TitleRole:
        return session.title;
    case CreatedAtRole:
        return session.createdAt;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ChatSessionListModel::roleNames() const {
    return {
        {IdRole, "sessionId"},
        {TitleRole, "title"},
        {CreatedAtRole, "createdAt"},
    };
}

void ChatSessionListModel::setSessions(const QVector<ChatSession> &sessions) {
    beginResetModel();
    m_sessions = sessions;
    endResetModel();
}

QString ChatSessionListModel::titleForId(qint64 sessionId) const {
    for (const ChatSession &session : m_sessions) {
        if (session.id == sessionId) {
            return session.title;
        }
    }
    return QString();
}
