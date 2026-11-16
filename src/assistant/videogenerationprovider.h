/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QUrl>

struct VideoGenerationRequest {
    QString prompt;
    QString firstFramePath;
    QString ratio{QStringLiteral("16:9")};
    int duration{5};
    QString resolution{QStringLiteral("720p")};
};

class VideoGenerationProvider : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QString id() const = 0;
    virtual bool requiresCredential() const = 0;
    virtual bool supportsResume() const = 0;
    virtual bool configure(const QString &credential) = 0;
    virtual void create(const VideoGenerationRequest &request) = 0;
    virtual void cancelOrStopWaiting() = 0;

Q_SIGNALS:
    void status(const QString &state, const QString &message);
    void outputReady(const QString &taskId, const QUrl &downloadUrl, const QJsonObject &usage);
    void failed(const QString &message);
};
