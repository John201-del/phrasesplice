/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QObject>
#include <QJsonObject>
#include <QProcess>

class AvatarGenerator : public QObject
{
    Q_OBJECT
public:
    explicit AvatarGenerator(QObject *parent = nullptr);
    ~AvatarGenerator() override;
    void configure(const QString &ffmpeg, const QString &ffprobe, const QString &python, const QString &script,
                   const QString &runtimeRoot, const QString &outputRoot);
    void generate(const QString &imagePath, const QString &audioPath);
    void cancel();
    bool isBusy() const { return m_busy; }
Q_SIGNALS:
    void progress(const QString &message);
    void finished(const QJsonObject &result);
    void failed(const QString &message);
private:
    void fail(const QString &message);
    void stop();
    void cleanup();
    void probe(const QString &path, int stage);
    void consume();
    void completed(int code, QProcess::ExitStatus status);
    bool unchanged() const;
    QString m_ffmpeg, m_ffprobe, m_python, m_script, m_runtime, m_outputRoot;
    QString m_image, m_audio, m_output, m_jobDirectory;
    QByteArray m_imageSnapshot, m_audioSnapshot, m_stdout;
    QProcess m_process;
    double m_duration = 0;
    int m_stage = 0;
    qint64 m_received = 0;
    bool m_busy = false, m_cancelled = false, m_reportedOk = false, m_overflow = false;
    quintptr m_job = 0;
};
