#include "DocumentListModel.h"

DocumentListModel::DocumentListModel(QObject *parent) : QAbstractListModel(parent) {
}

int DocumentListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_names.size();
}

QVariant DocumentListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_names.size()) {
        return QVariant();
    }
    if (role == NameRole) {
        return m_names.at(index.row());
    }
    return QVariant();
}

QHash<int, QByteArray> DocumentListModel::roleNames() const {
    return {
        {NameRole, "name"},
    };
}

void DocumentListModel::setDocumentNames(const QVector<QString> &names) {
    beginResetModel();
    m_names = names;
    endResetModel();
}
