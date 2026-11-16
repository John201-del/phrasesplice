/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "../../src/assistant/codexclient.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    CodexClient client;
    int calls = 0;
    int textCharacters = 0;
    QString reply;
    QObject::connect(&client, &CodexClient::ready, &app, [&] {
        client.submit(QStringLiteral("请调用一次 editor_probe 工具，收到成功结果后，用中文简短确认。"));
    });
    QObject::connect(&client, &CodexClient::textDelta, &app, [&](const QString &text) { textCharacters += text.size(); reply += text; });
    QObject::connect(&client, &CodexClient::toolRequested, &app,
        [&](const QJsonValue &token, const QString &name, const QJsonObject &) {
            if (name != QStringLiteral("editor_probe")) {
                client.replyToTool(token, QJsonObject{{"ok", false}}, false);
                return;
            }
            ++calls;
            client.replyToTool(token, QJsonObject{{"ok", true}, {"message", QStringLiteral("本地工具往返成功，未修改任何工程。")}}, true);
        });
    QObject::connect(&client, &CodexClient::failed, &app, [&](const QString &message) {
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"ok", false}, {"message", message}}).toJson(QJsonDocument::Compact) << Qt::endl;
        app.exit(1);
    });
    QObject::connect(&client, &CodexClient::turnFinished, &app, [&](bool success) {
        const bool passed = success && calls == 1 && textCharacters > 0;
        QJsonObject result{{"ok", passed}, {"toolCalls", calls}, {"replyCharacters", textCharacters}};
        if (!passed) result.insert(QStringLiteral("syntheticProbeReply"), reply);
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        app.exit(passed ? 0 : 1);
    });
    QTimer::singleShot(180000, &app, [&] { client.stop(); app.exit(3); });
    const QJsonArray tools{QJsonObject{{"type", "function"}, {"deferLoading", false}, {"name", "editor_probe"}, {"description", "Verify the local editor tool connection without editing files."},
        {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}, {"additionalProperties", false}}}}};
    QTimer::singleShot(0, &app, [&] {
        client.start(app.arguments().at(1), app.arguments().at(2), tools,
            QStringLiteral("你是本地视频剪辑助手。本次只验证 editor_probe 工具往返。不要读取或修改文件，不要使用其他工具。"));
    });
    return app.exec();
}
