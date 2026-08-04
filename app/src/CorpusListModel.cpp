#include "CorpusListModel.h"

CorpusListModel::CorpusListModel(QObject *parent) : QAbstractListModel(parent) {
}

int CorpusListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_corpora.size();
}

QVariant CorpusListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_corpora.size()) {
        return QVariant();
    }
    const Corpus &corpus = m_corpora.at(index.row());
    switch (role) {
    case IdRole:
        return corpus.id;
    case DisplayNameRole:
        return corpus.displayName;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> CorpusListModel::roleNames() const {
    return {
        {IdRole, "corpusId"},
        {DisplayNameRole, "displayName"},
    };
}

void CorpusListModel::setCorpora(const QVector<Corpus> &corpora) {
    beginResetModel();
    m_corpora = corpora;
    endResetModel();
}
