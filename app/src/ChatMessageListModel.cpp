#include "ChatMessageListModel.h"

ChatMessageListModel::ChatMessageListModel(QObject *parent) : QAbstractListModel(parent) {
}

int ChatMessageListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_messages.size();
}

QVariant ChatMessageListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size()) {
        return QVariant();
    }
    const ChatMessage &message = m_messages.at(index.row());
    switch (role) {
    case TextRole:
        return message.text;
    case IsUserRole:
        return message.isUser;
    case SourcesRole:
        return message.sources;
    case IsFreshRole:
        return message.isFresh;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ChatMessageListModel::roleNames() const {
    return {
        {TextRole, "text"},
        {IsUserRole, "isUser"},
        {SourcesRole, "sources"},
        {IsFreshRole, "isFresh"},
    };
}

void ChatMessageListModel::addMessage(const QString &text, bool isUser, const QVariantList &sources) {
    int row = m_messages.size();
    beginInsertRows(QModelIndex(), row, row);
    m_messages.append({text, isUser, sources, /*isFresh=*/true});
    endInsertRows();
}

void ChatMessageListModel::setMessages(const QVector<ChatMessage> &messages) {
    beginResetModel();
    m_messages = messages;
    endResetModel();
}
