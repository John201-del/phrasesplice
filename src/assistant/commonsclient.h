/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QObject>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <memory>
class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryFile;
class CommonsClient : public QObject
{
    Q_OBJECT
public:
    explicit CommonsClient(const QString &outputDirectory, QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~CommonsClient() override;
    bool search(const QString &query);
    bool download(const QString &assetId);
    void cancel();
Q_SIGNALS:
    void searchFinished(const QJsonArray &assets);
    void downloaded(const QString &localPath, const QJsonObject &metadata);
    void failed(const QString &code);
    void canceled();
private:
    void consume();
    void completed();
    void clearRequest();
    void fail(const QString &code);
    void get(const QUrl &url);
    QString m_directory;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QMap<QString, QJsonObject> m_assets;
    QJsonObject m_metadata;
    QByteArray m_body;
    std::unique_ptr<QTemporaryFile> m_partial;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_received = 0;
    bool m_downloading = false;
};
