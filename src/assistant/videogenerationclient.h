/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QUrl>
#include <functional>
#include <memory>

class VideoGenerationProvider;

class VideoGenerationClient : public QObject
{
    Q_OBJECT
public:
    explicit VideoGenerationClient(QObject *parent = nullptr);
    ~VideoGenerationClient() override;

    // The key stays in memory. storageDirectory must resolve inside the current workspace.
    bool configure(const QString &apiKey, const QString &storageDirectory, const QString &ffprobePath);
    bool configureProvider(const QString &providerId, const QString &credential, const QString &storageDirectory, const QString &ffprobePath);
    QString providerId() const;
    bool supportsResume() const;
    void create(const QString &text, const QString &selectedFirstFramePath = {}, const QString &ratio = QStringLiteral("16:9"), int duration = 5,
                const QString &resolution = QStringLiteral("720p"));
    void resume(const QString &taskId);
    void refresh();
    void cancelOrStopWaiting();
    QString taskId() const;
    QString currentStatus() const;
    QString taskRecordPath() const;

Q_SIGNALS:
    // States include unconnected, idle, querying, submitting, submission_unknown, queued, cancelling,
    // running, stopped, downloading, validating, ready, cancelled, failed, expired.
    void status(const QString &state, const QString &message);
    void progress(qint64 receivedBytes, qint64 totalBytes);
    void ready(const QString &localPath);
    void failed(const QString &message);

private:
    friend class VideoGenerationClientTest;
    struct State;
    std::unique_ptr<State> d;
    // Local test injection is private; the product has a fixed official endpoint.
    QUrl m_apiBase{QStringLiteral("https://ark.cn-beijing.volces.com/api/v3")};
    std::function<std::unique_ptr<VideoGenerationProvider>()> m_providerFactory;
    int m_pollIntervalMs{5000};
    int m_retryIntervalMs{1000};
    int m_requestTimeoutMs{30000};
    int m_probeTimeoutMs{30000};
    void reset();
    void resetConfiguration(const QString &providerId);
    bool configureStorage(const QString &storageDirectory, const QString &ffprobePath);
    QString taskRecordPath(const QString &taskId) const;
    void setStatus(const QString &state, const QString &message = {});
    void fail(const QString &message);
    bool persist();
    void query(int attempt = 0);
    void download(const QUrl &url, int attempt = 0);
    void validate();
};
