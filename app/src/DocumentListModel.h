// Backs GroupDocumentsView.qml's ListView -- one row per document in
// the currently active group. Never created directly from QML
// (QML_UNCREATABLE); AppController owns the one instance and exposes it
// via its documentModel property.
//
// A document row is a name plus the stats a large-corpus user needs at
// a glance: how many passages the document indexed into and how many
// tokens those carry (from pg_store_list_document_stats()'s single
// GROUP BY, so populating a 2,000-document group costs one round trip,
// not 2,000).

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

    // Replaces the whole list and notifies bound views. Called by
    // AppController after anything that changes the group's document
    // set (group switch, ingest, removal).
    void setDocuments(const QVector<DocumentEntry> &documents);

private:
    QVector<DocumentEntry> m_documents;
};

#endif // LEXIS_APP_DOCUMENTLISTMODEL_H