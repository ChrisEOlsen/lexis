// Backs GroupContentView.qml's ListView -- one row per document name in
// the currently active group. Never created directly from QML
// (QML_UNCREATABLE); AppController owns the one instance and exposes it
// via its documentModel property.

#ifndef LEXIS_APP_DOCUMENTLISTMODEL_H
#define LEXIS_APP_DOCUMENTLISTMODEL_H

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>
#include <QVector>

class DocumentListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use AppController.documentModel")

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
    };

    explicit DocumentListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setDocumentNames(const QVector<QString> &names);

private:
    QVector<QString> m_names;
};

#endif // LEXIS_APP_DOCUMENTLISTMODEL_H
