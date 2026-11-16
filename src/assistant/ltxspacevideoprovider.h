/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "videogenerationprovider.h"

#include <QNetworkAccessManager>
#include <QTimer>

class QNetworkReply;

class LtxSpaceVideoProvider : public VideoGenerationProvider
{
public:
    explicit LtxSpaceVideoProvider(QObject *parent = nullptr);
    QString id() const override;
    bool requiresCredential() const override;
    bool supportsResume() const override;
    bool configure(const QString &credential) override;
    void create(const VideoGenerationRequest &request) override;
    void cancelOrStopWaiting() override;

private:
    friend class LtxSpaceVideoProviderTest;

    void clearReply();
    void fail(const QString &message);
    void failQuota();
    void waitForEvent(const QString &taskId, quint64 generation);

    QUrl m_apiBase{QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api")};
    int m_requestTimeoutMs{60000};
    int m_overallTimeoutMs{720000};
    quint64 m_generation{0};
    QNetworkAccessManager m_network;
    QTimer m_overallTimer;
    QNetworkReply *m_reply{nullptr};
};
