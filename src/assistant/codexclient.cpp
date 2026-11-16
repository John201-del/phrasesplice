#include "codexclient.h"
#include "workspacepaths.h"

#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace {
const QStringList disabledFeatures{
    "shell_tool", "plugins", "apps", "remote_plugin", "hooks", "multi_agent",
    "computer_use", "browser_use", "in_app_browser", "image_generation",
    "view_image", "code_mode"
};

QString quoted(const QString &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

QString safeFailure(const QJsonObject &error)
{
    auto info = error.value("codexErrorInfo");
    if (info.isUndefined()) info = error.value("data").toObject().value("codexErrorInfo");
    QString kind = info.toString();
    int httpStatus = 0;
    if (info.isObject()) {
        const auto details = info.toObject();
        for (const QString &name : {QStringLiteral("httpConnectionFailed"), QStringLiteral("responseStreamConnectionFailed"),
                                    QStringLiteral("responseStreamDisconnected")}) {
            if (details.contains(name)) {
                kind = name;
                httpStatus = details.value(name).toObject().value("httpStatusCode").toInt();
                break;
            }
        }
    }
    // Only map structured error kinds; never forward remote messages, prompts or credentials.
    if (kind == "modelNotFound") return QStringLiteral("当前 Codex 模型不可用，请重新连接以刷新可用模型；本轮编辑不会自动重放。");
    if (kind == "usageLimitExceeded" || kind == "rateLimitExceeded" || httpStatus == 429)
        return QStringLiteral("Codex 额度或速率受限，请稍后再试或检查账户额度；本轮编辑不会自动重放。");
    if (kind == "unauthorized" || httpStatus == 401 || httpStatus == 403)
        return QStringLiteral("Codex 登录或访问权限失效，请在 Codex 中重新登录后再连接；本轮编辑不会自动重放。");
    if (kind == "contextWindowExceeded") return QStringLiteral("本轮上下文过长，请重新连接并缩小剪辑要求；本轮编辑不会自动重放。");
    if (kind == "serverOverloaded" || httpStatus == 503)
        return QStringLiteral("Codex 服务繁忙，请稍后重新连接；本轮编辑不会自动重放。");
    if (kind == "streamDisconnected" || kind == "streamConnectionFailed" || kind == "httpConnectionFailed"
        || kind == "responseStreamConnectionFailed" || kind == "responseStreamDisconnected")
        return QStringLiteral("Codex 网络连接中断，请检查网络后重新连接；请先检查当前工程，本轮编辑不会自动重放。");
    return QStringLiteral("Codex 本轮处理失败，请检查连接和账户后重新连接；请先检查当前工程，本轮编辑不会自动重放。");
}
}

CodexClient::CodexClient(QObject *parent) : QObject(parent)
{
    m_requestTimeout.setSingleShot(true);
    m_requestTimeout.setInterval(30000);
    connect(&m_requestTimeout, &QTimer::timeout, this, [this] { fail(QStringLiteral("Codex protocol request timed out.")); });
    m_cancelTimeout.setSingleShot(true);
    m_cancelTimeout.setInterval(5000);
    connect(&m_cancelTimeout, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("Codex 未及时完成停止，已断开连接。可以重新连接后继续。"));
    });
}

CodexClient::~CodexClient()
{
    releaseProcess();
}

void CodexClient::start(const QString &executable, const QString &workingDirectory,
                        const QJsonArray &tools, const QString &instructions,
                        const QString &model, const QStringList &argumentPrefix)
{
    stop();
    m_directory = QDir::fromNativeSeparators(QDir(workingDirectory).canonicalPath());
    if (m_directory.isEmpty() || !WorkspacePaths::isInside(workingDirectory)) {
        Q_EMIT failed(QStringLiteral("Codex working directory must exist inside the current workspace without links."));
        return;
    }
    m_runtimeDirectory = m_directory + "/.codex-client";
    for (const QString &name : {QStringLiteral("logs"), QStringLiteral("state"), QStringLiteral("tmp")}) {
        if (!WorkspacePaths::isInside(m_runtimeDirectory + '/' + name) || !QDir().mkpath(m_runtimeDirectory + '/' + name)) {
            Q_EMIT failed(QStringLiteral("Cannot create Codex workspace runtime directories."));
            return;
        }
    }
    m_executable = executable;
    m_arguments = argumentPrefix;
    m_arguments << "app-server" << "--stdio";
    for (const QString &feature : disabledFeatures) m_arguments << "-c" << "features." + feature + "=false";
    m_arguments << "-c" << "features.code_mode_host=true"
                << "-c" << "mcp_servers={}" << "-c" << "web_search=\"disabled\""
                << "-c" << "log_dir=" + quoted(m_runtimeDirectory + "/logs")
                << "-c" << "sqlite_home=" + quoted(m_runtimeDirectory + "/state");
    m_threadParams = {{"cwd", m_directory}, {"developerInstructions", instructions},
        {"dynamicTools", tools}, {"environments", QJsonArray{}}, {"sandbox", "read-only"},
        {"approvalPolicy", "never"}, {"ephemeral", true}};
    if (!model.isEmpty()) m_threadParams.insert("model", model);
    m_restarted = false;
    m_disabledServers.clear();
    m_inventoryCursors.clear();
    launch();
}

void CodexClient::launch()
{
    releaseProcess();
    m_modelCursors.clear();
    m_catalogDefaultModel.clear();
    auto *process = new QProcess(this);
    m_process = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("TEMP", m_runtimeDirectory + "/tmp");
    environment.insert("TMP", m_runtimeDirectory + "/tmp");
    environment.insert("RUST_LOG", "off");
    environment.insert("PYTHONDONTWRITEBYTECODE", "1");
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(m_directory);
    connect(process, &QProcess::started, this, [this, process] {
        if (m_process != process) return;
        request("initialize", {{"clientInfo", QJsonObject{{"name", "kdenlive-assistant"}, {"version", "0.1"}}},
            {"capabilities", QJsonObject{{"experimentalApi", true}}}});
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (m_process != process) return;
        m_buffer += process->readAllStandardOutput();
        if (m_buffer.size() > 8 * 1024 * 1024) {
            fail(QStringLiteral("Codex protocol message exceeds the size limit."));
            return;
        }
        qsizetype newline;
        while (m_process == process && (newline = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(newline).trimmed();
            m_buffer.remove(0, newline + 1);
            if (line.isEmpty()) continue;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                fail(QStringLiteral("Codex sent an invalid JSON protocol message."));
                return;
            }
            receive(document.object());
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] {
        // Never forward raw server diagnostics: they may contain prompts or credentials.
        process->readAllStandardError();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process == process && error != QProcess::Crashed) fail(QStringLiteral("Codex process could not start or communicate."));
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
        if (m_process == process) fail(QStringLiteral("Codex process exited (code %1). Diagnostic content withheld.").arg(code));
    });
    QStringList arguments = m_arguments;
    for (const QString &name : m_disabledServers) arguments << "-c" << "mcp_servers." + name + ".enabled=false";
    Q_EMIT statusChanged(QStringLiteral("Starting Codex"));
    process->start(m_executable, arguments);
}

void CodexClient::releaseProcess()
{
    m_requestTimeout.stop();
    m_cancelTimeout.stop();
    m_pending.clear();
    m_buffer.clear();
    m_toolRequests.clear();
    if (!m_process) return;
    auto *process = m_process;
    m_process = nullptr;
    disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1000);
    }
    process->deleteLater();
}

void CodexClient::send(const QJsonObject &message)
{
    if (m_process && m_process->state() == QProcess::Running) {
        const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        if (m_process->write(bytes) != bytes.size()) fail(QStringLiteral("Cannot write to Codex process."));
    }
}

void CodexClient::request(const QString &method, const QJsonObject &params)
{
    const qint64 id = ++m_nextRequestId;
    m_pending.insert(id, method);
    m_requestTimeout.start();
    send({{"id", id}, {"method", method}, {"params", params}});
}

void CodexClient::checkConfiguration(const QJsonObject &config)
{
    const auto features = config.value("features").toObject();
    if (!features.value("code_mode_host").isBool() || !features.value("code_mode_host").toBool()) {
        fail(QStringLiteral("Codex local tool dispatcher is unavailable."));
        return;
    }
    for (const QString &feature : disabledFeatures) {
        if (!features.value(feature).isBool() || features.value(feature).toBool()) {
            fail(QStringLiteral("Codex tool isolation could not be verified."));
            return;
        }
    }
    if (config.value("web_search").toString() != "disabled"
        || QDir::cleanPath(config.value("log_dir").toString()).compare(m_runtimeDirectory + "/logs", Qt::CaseInsensitive) != 0
        || QDir::cleanPath(config.value("sqlite_home").toString()).compare(m_runtimeDirectory + "/state", Qt::CaseInsensitive) != 0) {
        fail(QStringLiteral("Codex workspace paths or web isolation could not be verified."));
        return;
    }
    const auto serversValue = config.value("mcp_servers");
    if (!serversValue.isUndefined() && !serversValue.isObject()) {
        fail(QStringLiteral("Codex MCP isolation configuration is invalid."));
        return;
    }
    const auto servers = serversValue.toObject();
    bool enabled = false;
    for (auto entry = servers.begin(); entry != servers.end(); ++entry) {
        const auto flag = entry.value().toObject().value("enabled");
        enabled |= !flag.isBool() || flag.toBool();
    }
    if (enabled) {
        if (m_restarted) {
            fail(QStringLiteral("Inherited Codex MCP servers could not be disabled."));
            return;
        }
        const QRegularExpression safeName("^[A-Za-z0-9_-]+$");
        for (auto entry = servers.begin(); entry != servers.end(); ++entry) {
            if (!safeName.match(entry.key()).hasMatch()) {
                fail(QStringLiteral("Inherited MCP server name needs unsupported configuration escaping."));
                return;
            }
            m_disabledServers.append(entry.key());
        }
        m_restarted = true;
        launch();
        return;
    }
    request("mcpServerStatus/list", {});
}

void CodexClient::response(const QString &method, const QJsonObject &result)
{
    if (method == "initialize") {
        send({{"method", "initialized"}});
        request("config/read", {{"includeLayers", false}});
    } else if (method == "config/read") {
        checkConfiguration(result.value("config").toObject());
    } else if (method == "mcpServerStatus/list") {
        if (!result.value("data").isArray()) {
            fail(QStringLiteral("Codex MCP tool inventory is invalid."));
            return;
        }
        for (const auto &entry : result.value("data").toArray()) {
            const auto tools = entry.toObject().value("tools");
            if (!tools.isObject() || !tools.toObject().isEmpty()) {
                fail(QStringLiteral("Codex still exposes unrelated MCP tools."));
                return;
            }
        }
        const auto cursor = result.value("nextCursor");
        if (cursor.isString()) {
            if (cursor.toString().isEmpty() || m_inventoryCursors.contains(cursor.toString())) {
                fail(QStringLiteral("Codex MCP inventory pagination did not advance."));
                return;
            }
            m_inventoryCursors.insert(cursor.toString());
            request("mcpServerStatus/list", {{"cursor", cursor}});
        } else if (m_threadParams.contains("model")) {
            request("thread/start", m_threadParams);
        } else {
            request("model/list", {{"limit", 100}, {"includeHidden", false}});
        }
    } else if (method == "model/list") {
        if (!result.value("data").isArray()) {
            fail(QStringLiteral("无法读取 Codex 可用模型，请更新或重启 Codex 后重新连接。"));
            return;
        }
        for (const auto &entry : result.value("data").toArray()) {
            const auto model = entry.toObject();
            if (!model.value("isDefault").toBool() || model.value("hidden").toBool()) continue;
            const QString id = model.value("model").toString();
            if (id.isEmpty() || (!m_catalogDefaultModel.isEmpty() && m_catalogDefaultModel != id)) {
                fail(QStringLiteral("Codex 可用模型没有唯一默认项，请检查 Codex 模型设置后重新连接。"));
                return;
            }
            m_catalogDefaultModel = id;
        }
        const auto cursor = result.value("nextCursor");
        if (cursor.isString()) {
            if (cursor.toString().isEmpty() || m_modelCursors.contains(cursor.toString()) || m_modelCursors.size() >= 10) {
                fail(QStringLiteral("Codex 可用模型列表分页异常，请更新或重启 Codex 后重新连接。"));
                return;
            }
            m_modelCursors.insert(cursor.toString());
            request("model/list", {{"limit", 100}, {"includeHidden", false}, {"cursor", cursor}});
        } else if ((!cursor.isNull() && !cursor.isUndefined()) || m_catalogDefaultModel.isEmpty()) {
            fail(QStringLiteral("Codex 可用模型没有默认项，请检查 Codex 模型设置后重新连接。"));
        } else {
            m_threadParams.insert("model", m_catalogDefaultModel);
            request("thread/start", m_threadParams);
        }
    } else if (method == "thread/start") {
        m_threadId = result.value("thread").toObject().value("id").toString();
        if (m_threadId.isEmpty()) {
            fail(QStringLiteral("Codex did not return a thread identifier."));
            return;
        }
        m_ready = true;
        Q_EMIT statusChanged(QStringLiteral("Ready"));
        Q_EMIT ready();
    } else if (method == "turn/start" && m_busy) {
        const auto turn = result.value("turn").toObject();
        m_turnId = turn.value("id").toString();
        if (m_turnId.isEmpty()) {
            fail(QStringLiteral("Codex did not return a turn identifier."));
            return;
        }
        if (m_cancelRequested) interrupt();
    }
}

void CodexClient::receive(const QJsonObject &message)
{
    if (!message.contains("method")) {
        if (!message.value("id").isDouble()) return;
        const QString method = m_pending.take(message.value("id").toInteger());
        if (method.isEmpty()) return;
        if (m_pending.isEmpty()) m_requestTimeout.stop();
        if (message.contains("error")) {
            fail(safeFailure(message.value("error").toObject()));
            return;
        }
        response(method, message.value("result").toObject());
        return;
    }
    const QString method = message.value("method").toString();
    const auto params = message.value("params").toObject();
    if (message.contains("id")) {
        if (method != "item/tool/call") {
            send({{"id", message.value("id")}, {"error", QJsonObject{
                {"code", -32601}, {"message", "Unsupported server request"}}}});
            if (m_process) m_process->waitForBytesWritten(100);
            fail(QStringLiteral("Codex requested an unsupported client operation."));
            return;
        }
        if (!m_busy || m_cancelRequested || params.value("threadId").toString() != m_threadId
            || params.value("turnId").toString() != m_turnId || !params.value("arguments").isObject()
            || params.value("tool").toString().isEmpty()) {
            send({{"id", message.value("id")}, {"error", QJsonObject{
                {"code", -32602}, {"message", "Invalid or inactive tool request"}}}});
            return;
        }
        const QString token = "codex-tool-" + QString::number(++m_nextToolToken);
        m_toolRequests.insert(token, message.value("id"));
        QString name = params.value("tool").toString();
        if (!params.value("namespace").toString().isEmpty()) name.prepend(params.value("namespace").toString() + '.');
        Q_EMIT toolRequested(token, name, params.value("arguments").toObject());
        return;
    }
    if (!m_busy || params.value("threadId").toString() != m_threadId) return;
    if (method == "turn/started") {
        const QString id = params.value("turn").toObject().value("id").toString();
        if (m_turnId.isEmpty()) m_turnId = id;
        if (m_cancelRequested) interrupt();
    } else if (method == "item/agentMessage/delta" && !m_turnId.isEmpty()
               && params.value("turnId").toString() == m_turnId) {
        Q_EMIT textDelta(params.value("delta").toString());
    } else if (method == "turn/completed") {
        const auto turn = params.value("turn").toObject();
        if (m_turnId.isEmpty() || turn.value("id").toString() != m_turnId) return;
        const QString status = turn.value("status").toString();
        if (status == "failed") {
            fail(safeFailure(turn.value("error").toObject()));
            return;
        }
        finishTurn(status == "completed" && !m_cancelRequested);
    }
}

void CodexClient::submit(const QString &text)
{
    if (!m_ready || m_busy) {
        Q_EMIT failed(m_busy ? QStringLiteral("Codex is already processing a turn.") : QStringLiteral("Codex is not ready."));
        return;
    }
    m_busy = true;
    m_turnId.clear();
    m_cancelRequested = false;
    m_interruptSent = false;
    Q_EMIT statusChanged(QStringLiteral("Working"));
    request("turn/start", {{"threadId", m_threadId},
        {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}});
}

void CodexClient::cancel()
{
    if (!m_busy || m_cancelRequested) return;
    m_cancelRequested = true;
    m_cancelTimeout.start();
    const auto requests = m_toolRequests;
    m_toolRequests.clear();
    for (const auto &id : requests) {
        send({{"id", id}, {"result", QJsonObject{{"success", false}, {"contentItems", QJsonArray{
            QJsonObject{{"type", "inputText"}, {"text", "Tool call cancelled"}}}}}}});
    }
    Q_EMIT statusChanged(QStringLiteral("Cancelling"));
    interrupt();
}

void CodexClient::interrupt()
{
    if (!m_busy || m_turnId.isEmpty() || m_interruptSent) return;
    m_interruptSent = true;
    request("turn/interrupt", {{"threadId", m_threadId}, {"turnId", m_turnId}});
}

void CodexClient::replyToTool(const QJsonValue &requestId, const QJsonObject &result, bool success)
{
    replyToTool(requestId, QJsonArray{QJsonObject{{"type", "inputText"},
        {"text", QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}}}, success);
}

void CodexClient::replyToTool(const QJsonValue &requestId, const QJsonArray &contentItems, bool success)
{
    if (!requestId.isString() || !m_toolRequests.contains(requestId.toString()) || !m_busy || !m_process) {
        Q_EMIT failed(QStringLiteral("Tool reply has no active Codex request."));
        return;
    }
    for (const auto &value : contentItems) {
        const auto item = value.toObject();
        const QString type = item.value("type").toString();
        const QString field = type == "inputText" ? "text" : type == "inputImage" ? "imageUrl" : type == "inputAudio" ? "audioUrl" : QString();
        if (!value.isObject() || field.isEmpty() || !item.value(field).isString()) {
            Q_EMIT failed(QStringLiteral("Tool reply contains an unsupported content item."));
            return;
        }
    }
    const auto id = m_toolRequests.take(requestId.toString());
    send({{"id", id}, {"result", QJsonObject{{"contentItems", contentItems}, {"success", success}}}});
}

void CodexClient::finishTurn(bool success)
{
    if (!m_busy) return;
    m_cancelTimeout.stop();
    m_busy = false;
    m_turnId.clear();
    m_cancelRequested = false;
    m_interruptSent = false;
    m_toolRequests.clear();
    for (auto entry = m_pending.begin(); entry != m_pending.end();) {
        if (entry.value() == "turn/start" || entry.value() == "turn/interrupt") entry = m_pending.erase(entry);
        else ++entry;
    }
    if (m_pending.isEmpty()) m_requestTimeout.stop();
    Q_EMIT statusChanged(m_ready ? QStringLiteral("Ready") : QStringLiteral("Stopped"));
    Q_EMIT turnFinished(success);
}

void CodexClient::stop()
{
    releaseProcess();
    m_ready = false;
    m_threadId.clear();
    finishTurn(false);
    Q_EMIT statusChanged(QStringLiteral("Stopped"));
}

void CodexClient::fail(const QString &message)
{
    stop();
    Q_EMIT failed(message);
}

bool CodexClient::isReady() const { return m_ready; }
bool CodexClient::isBusy() const { return m_busy; }
