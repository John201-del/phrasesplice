/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <QCoreApplication>
#include <QFile>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    QString output = arguments.constLast();
    const int consumer = arguments.indexOf(QStringLiteral("-consumer"));
    if (consumer >= 0 && consumer + 1 < arguments.size()) {
        output = arguments.at(consumer + 1);
        output.remove(0, QStringLiteral("avformat:").size());
    }
    QFile partial(output);
    if (!partial.open(QIODevice::WriteOnly) || partial.write("partial-transcode-output") <= 0) return 18;
    return 17;
}
