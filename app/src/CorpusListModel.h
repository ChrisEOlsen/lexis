// Backs GroupSidebar.qml's ListView -- one row per group. Never created
// directly from QML (QML_UNCREATABLE); AppController owns the one
// instance and exposes it via its corpusModel property.

#ifndef LEXIS_APP_CORPUSLISTMODEL_H
#define LEXIS_APP_CORPUSLISTMODEL_H

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QVector>

#include "LexisEngine.h" // for the Corpus struct

class CorpusListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use AppController.corpusModel")

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        DisplayNameRole,
    };

    explicit CorpusListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the whole list and notifies bound QML views. Called by
    // AppController after any operation that could change the set of
    // groups (create, delete, or the initial load).
    void setCorpora(const QVector<Corpus> &corpora);

private:
    QVector<Corpus> m_corpora;
};

#endif // LEXIS_APP_CORPUSLISTMODEL_H
