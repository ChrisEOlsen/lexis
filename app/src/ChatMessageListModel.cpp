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
    case ToolRole:
        return message.tool;
    case SearchQueryRole:
        return message.searchQuery;
    case SearchTermsRole:
        return message.searchTerms;
    case IsLiveRole:
        return message.isLive;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ChatMessageListModel::roleNames() const {
    return {
        {TextRole, "text"},
        {IsUserRole, "isUser"},
        {SourcesRole, "sources"},
        {ToolRole, "tool"},
        {SearchQueryRole, "searchQuery"},
        {SearchTermsRole, "searchTerms"},
        {IsLiveRole, "isLive"},
    };
}

void ChatMessageListModel::addMessage(const QString &text, bool isUser, const QVariantList &sources,
                                      const QString &tool, const QString &searchQuery,
                                      const QString &searchTerms) {
    int row = m_messages.size();
    beginInsertRows(QModelIndex(), row, row);
    m_messages.append({text, isUser, sources, tool, searchQuery, searchTerms, /*isLive=*/false});
    endInsertRows();
}

void ChatMessageListModel::setMessages(const QVector<ChatMessage> &messages) {
    beginResetModel();
    m_messages = messages;
    // A reset can never leave a live row: the only caller loads a whole
    // session's history, all finished messages.
    m_liveIndex = -1;
    endResetModel();
}

void ChatMessageListModel::beginLiveAnswer() {
    if (m_liveIndex >= 0) {
        return; // one live row at a time, by the one-query-at-a-time rule
    }
    int row = m_messages.size();
    beginInsertRows(QModelIndex(), row, row);
    m_messages.append({QString(), false, QVariantList(), QString(), QString(), QString(), /*isLive=*/true});
    endInsertRows();
    m_liveIndex = row;
}

void ChatMessageListModel::makeLastAssistantLive() {
    if (m_liveIndex >= 0) {
        return; // already streaming
    }
    if (m_messages.isEmpty() || m_messages.last().isUser) {
        return; // nothing to convert
    }
    int row = m_messages.size() - 1;
    ChatMessage &message = m_messages[row];
    message.text.clear();
    message.sources = QVariantList();
    message.tool.clear();
    message.searchQuery.clear();
    message.searchTerms.clear();
    message.isLive = true;
    m_liveIndex = row;
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {TextRole, SourcesRole, ToolRole, SearchQueryRole, SearchTermsRole, IsLiveRole});
}

void ChatMessageListModel::restoreLastAssistant(const QString &text, const QVariantList &sources, const QString &tool,
                                                const QString &searchQuery, const QString &searchTerms) {
    if (m_liveIndex < 0) {
        return;
    }
    ChatMessage &message = m_messages[m_liveIndex];
    message.text = text;
    message.sources = sources;
    message.tool = tool;
    message.searchQuery = searchQuery;
    message.searchTerms = searchTerms;
    message.isLive = false;
    QModelIndex row = index(m_liveIndex);
    m_liveIndex = -1;
    emit dataChanged(row, row, {TextRole, SourcesRole, ToolRole, SearchQueryRole, SearchTermsRole, IsLiveRole});
}

void ChatMessageListModel::appendLiveText(const QString &piece) {
    if (m_liveIndex < 0 || piece.isEmpty()) {
        return;
    }
    m_messages[m_liveIndex].text.append(piece);
    QModelIndex row = index(m_liveIndex);
    emit dataChanged(row, row, {TextRole});
}

void ChatMessageListModel::resetLiveText() {
    if (m_liveIndex < 0) {
        return;
    }
    m_messages[m_liveIndex].text.clear();
    QModelIndex row = index(m_liveIndex);
    emit dataChanged(row, row, {TextRole});
}

void ChatMessageListModel::finishLive(const QString &text, const QVariantList &sources, const QString &tool,
                                      const QString &searchQuery, const QString &searchTerms) {
    if (m_liveIndex < 0) {
        // No live row was ever created (no tokens arrived -- e.g. an
        // empty reply): the finished answer is just an ordinary append.
        addMessage(text, false, sources, tool, searchQuery, searchTerms);
        return;
    }
    ChatMessage &message = m_messages[m_liveIndex];
    message.text = text;
    message.sources = sources;
    message.tool = tool;
    message.searchQuery = searchQuery;
    message.searchTerms = searchTerms;
    message.isLive = false;
    QModelIndex row = index(m_liveIndex);
    m_liveIndex = -1;
    emit dataChanged(row, row, {TextRole, SourcesRole, ToolRole, SearchQueryRole, SearchTermsRole, IsLiveRole});
}

void ChatMessageListModel::discardLive() {
    if (m_liveIndex < 0) {
        return;
    }
    beginRemoveRows(QModelIndex(), m_liveIndex, m_liveIndex);
    m_messages.remove(m_liveIndex);
    endRemoveRows();
    m_liveIndex = -1;
}