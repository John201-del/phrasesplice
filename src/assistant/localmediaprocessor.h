#pragma once
#include <QObject>
#include <QJsonObject>
#include <QProcess>

class LocalMediaProcessor : public QObject
{
    Q_OBJECT
public:
    explicit LocalMediaProcessor(QObject *parent = nullptr);
    ~LocalMediaProcessor() override;
    void configure(const QString &ffmpeg, const QString &ffprobe, const QString &python, const QString &script,
                   const QString &modelRoot, const QString &runtimeRoot, const QString &outputRoot);
    void process(const QString &sourcePath, const QString &operation, const QString &stickerPath = {});
    void retouchPhoto(const QString &sourcePath, const QString &destination, const QJsonObject &options);
    void cancel();
    bool isBusy() const { return m_busy; }
Q_SIGNALS:
    void progress(const QString &message);
    void finished(const QJsonObject &result);
    void failed(const QString &message);
private:
    void fail(const QString &message);
    bool prepare(const QString &sourcePath, const QString &operation);
    void consumeReport(const QJsonObject &report);
    void startProbe(const QString &path, bool output);
    void startProcessing();
    void completed(int code, QProcess::ExitStatus status);
    QString m_ffmpeg, m_ffprobe, m_python, m_script, m_models, m_runtime, m_outputRoot;
    QString m_source, m_operation, m_output, m_background;
    QByteArray m_stdout, m_sourceSnapshot;
    QJsonObject m_sourceMetadata, m_outputMetadata;
    QString m_photoDestination, m_extraSource, m_photoError;
    QJsonObject m_photoOptions, m_photoReport;
    QByteArray m_extraSourceSnapshot;
    QProcess m_process;
    bool m_busy = false, m_cancelled = false, m_reportedOk = false;
    int m_stage = 0;
};
