// Controlled FFmpeg fixture for cancellation and an unclosed trailing silence.
#include <QCoreApplication>
#include <QFile>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("-af")) && !application.arguments().contains(QStringLiteral("-c:a"))) {
        QFile error;
        if (!error.open(stderr, QIODevice::WriteOnly)) {
            return 2;
        }
        error.write("[silencedetect] silence_start: 0.5\n");
        return 0;
    }
    QFile marker(QStringLiteral("extract-running"));
    if (!marker.open(QIODevice::WriteOnly)) {
        return 2;
    }
    marker.write("running");
    marker.close();
    QThread::sleep(15);
    return 1;
}
