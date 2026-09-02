#include "DocumentListModel.h"

DocumentListModel::DocumentListModel(QObject *parent) : QAbstractListModel(parent) {
}

int DocumentListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_documents.size();
}

QVariant DocumentListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_documents.size()) {
        return QVariant();
    }
    const DocumentEntry &entry = m_documents.at(index.row());
    switch (role) {
    case NameRole:
        return entry.name;
    case PassageCountRole:
        return entry.passageCount;
    case TokenCountRole:
        return entry.tokenCount;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> DocumentListModel::roleNames() const {
    return {
        {NameRole, "name"},
        {PassageCountRole, "passageCount"},
        {TokenCountRole, "tokenCount"},
    };
}

void DocumentListModel::setDocuments(const QVector<DocumentEntry> &documents) {
    beginResetModel();
    m_documents = documents;
    endResetModel();
}