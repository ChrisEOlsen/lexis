// One row per document in the active group (QML_UNCREATABLE; owned by AppController).
// Rows carry name + passage/token stats from pg_store_list_document_stats()' single round trip.

#ifndef LEXIS_APP_DOCUMENTLISTMODEL_H
#define LEXIS_APP_DOCUMENTLISTMODEL_H

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVector>

struct DocumentEntry {
    QString name;
    qlonglong passageCount = 0;
    qlonglong tokenCount = 0;
};

class DocumentListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use AppController.documentModel")

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PassageCountRole,
        TokenCountRole,
    };

    explicit DocumentListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the whole list. Called after group switch, ingest, or removal.
    void setDocuments(const QVector<DocumentEntry> &documents);

private:
    QVector<DocumentEntry> m_documents;
};

#endif // LEXIS_APP_DOCUMENTLISTMODEL_H