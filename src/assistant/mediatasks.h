// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>

class QProcess;

class MediaTasks : public QObject
{
    Q_OBJECT
public:
    explicit MediaTasks(QObject *parent = nullptr);
    ~MediaTasks() override;
    void configure(const QString &ffmpegPath, const QString &ffprobePath, const QString &pythonPath, const QString &whisperScript,
                   const QString &cacheRoot);
    void sampleFrame(const QString &sourcePath, double seconds);
    void detectSilence(const QString &sourcePath);
    void transcribe(const QString &sourcePath, const QString &language = QString());
    void cancel();
    bool isBusy() const;

Q_SIGNALS:
    void progress(const QString &message);
    void finished(const QJsonObject &result, const QByteArray &image);
    void failed(const QString &message);

private:
    bool begin(const QString &sourcePath);
    void run(const QString &program, const QStringList &arguments, const std::function<void(QByteArray, QByteArray)> &next, bool capture = true);
    void probe(const QString &path, const std::function<void(QJsonObject)> &next);
    void fail(const QString &message);
    void complete(const QJsonObject &result, const QByteArray &image = {});
    bool prepareAudio(const QJsonObject &info);
    QString audioFilter() const;
    QJsonObject audioResult(const QString &kind) const;
    void readTranscript();
    QString m_ffmpeg, m_ffprobe, m_python, m_script, m_cache, m_job, m_source;
    QProcess *m_process = nullptr;
    quint64 m_generation = 0;
    bool m_busy = false;
    double m_duration = 0;
    double m_audioOffset = 0, m_audioDuration = 0, m_audioTrim = 0;
    int m_audioStreamIndex = -1;
};
