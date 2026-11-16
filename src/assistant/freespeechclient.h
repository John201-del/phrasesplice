/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QSaveFile;
class QCryptographicHash;

class FreeSpeechClient : public QObject
{
    Q_OBJECT
public:
    explicit FreeSpeechClient(const QString &outputDirectory, const QString &ffprobe, QObject *parent = nullptr,
                              QNetworkAccessManager *network = nullptr);
    ~FreeSpeechClient() override;
    void generate(const QString &text);
    void cancel();
    bool isBusy() const;

Q_SIGNALS:
    void progress(const QString &message);
    void ready(const QString &path);
    void failed(const QString &message);

private:
    void fail(const QString &message);
    void clear();
    void submit(bool initialize);
    void events(bool initialize);
    void terminal(bool initialize, const QByteArray &data);
    void download(const QUrl &url);
    void probe();
    QNetworkReply *request(const QUrl &url, const QByteArray &payload = {});
    bool responseOk(QNetworkReply *reply);

    QString m_directory;
    QString m_ffprobe;
    QString m_text;
    QString m_session;
    QString m_output;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<QProcess> m_probe;
    std::unique_ptr<QSaveFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    QTimer m_deadline;
    bool m_busy{false};
    quint64 m_generation{0};
};
