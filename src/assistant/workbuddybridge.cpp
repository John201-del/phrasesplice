/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "workbuddybridge.h"
#include "workspacepaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QUuid>
#include <cmath>

namespace {
constexpr qint64 maximumMessage = 8 * 1024 * 1024;
bool validId(const QJsonValue &id)
{
    return id.isString() || (id.isDouble() && std::isfinite(id.toDouble()) && std::floor(id.toDouble()) == id.toDouble());
}
QByteArray idKey(const QJsonValue &id)
{
    return QCryptographicHash::hash(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
}

WorkBuddyBridge::WorkBuddyBridge(QObject *parent) : QObject(parent)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(1);
    connect(&m_server, &QLocalServer::newConnection, this, &WorkBuddyBridge::acceptConnection);
}

WorkBuddyBridge::~WorkBuddyBridge() { stop(); }

void WorkBuddyBridge::start(const QJsonArray &tools, const QString &instructions)
{
    stop();
    for (const auto &value : tools) {
        const auto tool = value.toObject();
        const QString name = tool.value("name").toString();
        if (name.isEmpty() || m_toolNames.contains(name) || !tool.value("inputSchema").isObject()) {
            Q_EMIT failed(QStringLiteral("WorkBuddy 工具清单无效。"));
            return;
        }
        m_toolNames.insert(name);
        m_tools.append(QJsonObject{{"name", name}, {"description", tool.value("description").toString()},
                                  {"inputSchema", tool.value("inputSchema")}});
    }
    m_instructions = instructions;
    QString root = QFileInfo(WorkspacePaths::root()).canonicalFilePath();
    if (root.isEmpty()) root = QDir::cleanPath(QFileInfo(WorkspacePaths::root()).absoluteFilePath());
#ifdef Q_OS_WIN
    root = root.toLower();
#endif
    m_socketName = QStringLiteral("localcut-mcp-") + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    // Windows permits multiple named-pipe listeners with the same name.
    const QString lockDirectory = WorkspacePaths::root() + QStringLiteral("/config");
    if (!WorkspacePaths::isInside(lockDirectory) || !QDir().mkpath(lockDirectory)) {
        Q_EMIT failed(QStringLiteral("无法创建 WorkBuddy 本地管道锁目录。"));
        return;
    }
    m_lock = std::make_unique<QLockFile>(lockDirectory + QStringLiteral("/localcut-mcp.lock"));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) {
        Q_EMIT failed(QStringLiteral("此数据目录已由另一个 PhraseSplice WorkBuddy 桥接实例使用。"));
        return;
    }
    if (!m_server.listen(m_socketName)) {
        m_lock.reset();
        Q_EMIT failed(QStringLiteral("无法启动 WorkBuddy 本地管道；请确认此数据目录没有另一个 PhraseSplice 实例：") + m_server.errorString());
        return;
    }
    Q_EMIT statusChanged(QStringLiteral("本地 MCP 已就绪，等待 WorkBuddy 连接。"));
    Q_EMIT ready();
}

void WorkBuddyBridge::stop()
{
    // Provider switching already cancels the controller. Do not re-enter it from here.
    m_toolToken.clear();
    m_toolId = QJsonValue();
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_server.close();
    m_lock.reset();
    m_buffer.clear();
    m_requestIds.clear();
    m_tools = {};
    m_toolNames.clear();
    m_initializeReceived = m_initialized = false;
}

bool WorkBuddyBridge::isReady() const { return m_server.isListening(); }
bool WorkBuddyBridge::isBusy() const { return !m_toolToken.isEmpty(); }

QJsonObject WorkBuddyBridge::configuration() const
{
    QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/localcut-mcp");
#ifdef Q_OS_WIN
    executable += QStringLiteral(".exe");
#endif
    return {{"mcpServers", QJsonObject{{"localcut", QJsonObject{{"type", "stdio"}, {"command", executable},
            {"args", QJsonArray{QStringLiteral("--socket"), m_socketName}}}}}}};
}

void WorkBuddyBridge::acceptConnection()
{
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        if (m_socket) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_socket = socket;
        socket->setReadBufferSize(maximumMessage + 1);
        m_buffer.clear();
        m_requestIds.clear();
        m_initializeReceived = m_initialized = false;
        connect(socket, &QLocalSocket::readyRead, this, &WorkBuddyBridge::readMessages);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            if (m_socket != socket) return;
            m_socket = nullptr;
            socket->deleteLater();
            const bool wasBusy = isBusy();
            m_toolToken.clear();
            m_toolId = QJsonValue();
            m_buffer.clear();
            m_initializeReceived = m_initialized = false;
            Q_EMIT cancelRequested();
            if (wasBusy) Q_EMIT turnFinished(false);
            Q_EMIT statusChanged(QStringLiteral("WorkBuddy 已断开，本地 MCP 等待重新连接。"));
        });
        if (socket->bytesAvailable()) readMessages();
    }
}

void WorkBuddyBridge::readMessages()
{
    const auto socket = m_socket;
    while (socket && socket == m_socket && socket->bytesAvailable()) {
        m_buffer += socket->read(maximumMessage + 1 - m_buffer.size());
        qsizetype newline;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            if (line.endsWith('\r')) line.chop(1);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(line, &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) error(QJsonValue(), -32700, QStringLiteral("Expected a JSON object."));
            else receive(document.object());
            if (!socket || socket != m_socket || socket->state() != QLocalSocket::ConnectedState) return;
        }
        if (m_buffer.size() > maximumMessage) {
            error(QJsonValue(), -32600, QStringLiteral("MCP message exceeds 8 MiB."));
            socket->disconnectFromServer();
            return;
        }
    }
}

bool WorkBuddyBridge::send(const QJsonObject &message)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) return false;
    const QByteArray data = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (data.size() > maximumMessage + 1) return false;
    if (m_socket->bytesToWrite() + data.size() > 2 * maximumMessage) {
        m_socket->abort();
        return false;
    }
    return m_socket->write(data) == data.size();
}

void WorkBuddyBridge::error(const QJsonValue &id, int code, const QString &message)
{
    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

void WorkBuddyBridge::receive(const QJsonObject &message)
{
    const bool request = message.contains("id");
    const auto id = message.value("id");
    const QString method = message.value("method").toString();
    if (message.value("jsonrpc") != QJsonValue(QStringLiteral("2.0")) || method.isEmpty() || (request && !validId(id))) {
        error(QJsonValue(), -32600, QStringLiteral("Invalid JSON-RPC request."));
        return;
    }
    const auto params = message.value("params").toObject();
    if (!request) {
        if (method == QLatin1String("notifications/initialized") && m_initializeReceived) m_initialized = true;
        else if (method == QLatin1String("notifications/cancelled") && isBusy() && params.value("requestId") == m_toolId) cancel();
        return;
    }
    if (m_requestIds.contains(idKey(id))) {
        // A duplicate pending ID cannot receive a second response under the same ID.
        error(QJsonValue(), -32600, QStringLiteral("Duplicate request id; reconnect with unique request ids."));
        if (m_socket) m_socket->disconnectFromServer();
        return;
    }
    // Bound per-connection bookkeeping even if a client never disconnects.
    if (m_requestIds.size() >= 65536) {
        error(id, -32000, QStringLiteral("Reconnect to start a new request-id session."));
        if (m_socket) m_socket->disconnectFromServer();
        return;
    }
    m_requestIds.insert(idKey(id));
    if (message.contains("params") && !message.value("params").isObject()) {
        error(id, -32602, QStringLiteral("Parameters must be an object."));
        return;
    }
    QJsonObject result;
    if (method == QLatin1String("initialize")) {
        if (m_initializeReceived || !params.value("protocolVersion").isString() || !params.value("capabilities").isObject()
            || !params.value("clientInfo").isObject()) {
            error(id, -32602, QStringLiteral("Invalid or repeated initialize request."));
            return;
        }
        const QString requested = params.value("protocolVersion").toString();
        const QString version = QStringList{QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"), QStringLiteral("2024-11-05")}.contains(requested)
            ? requested : QStringLiteral("2025-11-25");
        result = {{"protocolVersion", version}, {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                  {"serverInfo", QJsonObject{{"name", "LocalCut"}, {"version", QCoreApplication::applicationVersion().isEmpty()
                      ? QStringLiteral("1.0") : QCoreApplication::applicationVersion()}}}, {"instructions", m_instructions}};
        m_initializeReceived = true;
    } else if (method == QLatin1String("ping")) {
        result = {};
    } else if (!m_initialized) {
        error(id, -32002, QStringLiteral("Complete initialize and notifications/initialized before using tools."));
        return;
    } else if (method == QLatin1String("tools/list")) {
        if (params.contains("cursor")) {
            error(id, -32602, QStringLiteral("This tool catalog has no continuation cursor."));
            return;
        }
        result = {{"tools", m_tools}};
    } else if (method == QLatin1String("tools/call")) {
        const QString name = params.value("name").toString();
        if (!m_toolNames.contains(name) || (params.contains("arguments") && !params.value("arguments").isObject())) {
            error(id, -32602, QStringLiteral("Unknown tool or non-object arguments; consult tools/list."));
            return;
        }
        if (isBusy()) {
            error(id, -32000, QStringLiteral("A tool is already running; wait for its result or cancel it."));
            return;
        }
        m_toolId = id;
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_toolToken = token;
        Q_EMIT toolStarted();
        if (m_toolToken == token) Q_EMIT toolRequested(QJsonValue(token), name, params.value("arguments").toObject());
        return;
    } else {
        error(id, -32601, QStringLiteral("Method not found."));
        return;
    }
    if (!send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})) error(id, -32603, QStringLiteral("Response exceeds the transport limit."));
}

void WorkBuddyBridge::cancel()
{
    if (!isBusy()) return;
    const auto id = m_toolId;
    m_toolToken.clear();
    m_toolId = QJsonValue();
    Q_EMIT cancelRequested();
    error(id, -32800, QStringLiteral("Tool call cancelled."));
    Q_EMIT turnFinished(false);
}

void WorkBuddyBridge::replyToTool(const QJsonValue &token, const QJsonObject &result, bool success)
{
    replyToTool(token, QJsonArray{QJsonObject{{"type", "inputText"}, {"text", QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}}}, success);
}

void WorkBuddyBridge::replyToTool(const QJsonValue &token, const QJsonArray &contentItems, bool success)
{
    if (!token.isString() || m_toolToken.isEmpty() || token.toString() != m_toolToken) return;
    QJsonArray content;
    for (const auto &value : contentItems) {
        const auto item = value.toObject();
        const QString type = item.value("type").toString();
        if (type == QLatin1String("inputText") && item.value("text").isString()) {
            content.append(QJsonObject{{"type", "text"}, {"text", item.value("text")}});
        } else if (type == QLatin1String("inputImage") || type == QLatin1String("inputAudio")) {
            const QString url = item.value(type == QLatin1String("inputImage") ? "imageUrl" : "audioUrl").toString();
            const qsizetype marker = url.indexOf(QStringLiteral(";base64,"));
            const QString prefix = type == QLatin1String("inputImage") ? QStringLiteral("data:image/") : QStringLiteral("data:audio/");
            if (!url.startsWith(prefix) || marker < prefix.size()) {
                finish(QJsonArray{QJsonObject{{"type", "text"}, {"text", "Tool returned an unsupported media URL."}}}, false);
                return;
            }
            content.append(QJsonObject{{"type", type == QLatin1String("inputImage") ? "image" : "audio"},
                {"mimeType", url.mid(5, marker - 5)}, {"data", url.mid(marker + 8)}});
        } else {
            finish(QJsonArray{QJsonObject{{"type", "text"}, {"text", "Tool returned unsupported content."}}}, false);
            return;
        }
    }
    finish(content, success);
}

void WorkBuddyBridge::finish(const QJsonArray &content, bool success)
{
    const auto id = m_toolId;
    m_toolToken.clear();
    m_toolId = QJsonValue();
    if (!send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"content", content}, {"isError", !success}}}})) {
        error(id, -32603, QStringLiteral("Tool response exceeds the transport limit or the client disconnected."));
        success = false;
    }
    Q_EMIT turnFinished(success);
}
