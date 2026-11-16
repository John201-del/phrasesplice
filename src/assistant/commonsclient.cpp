/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "commonsclient.h"
#include "workspacepaths.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTextDocumentFragment>
#include <QUrlQuery>
#include <QUuid>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr qint64 maximumImageSize = 100LL * 1024 * 1024;
const QMap<QString, QString> extensions{{"image/jpeg", "jpg"}, {"image/png", "png"}, {"image/webp", "webp"},
    {"image/gif", "gif"}, {"image/tiff", "tiff"}, {"image/bmp", "bmp"}};
bool secureUrl(const QUrl &url, const QString &host) {
    return url.isValid() && url.scheme() == QStringLiteral("https") && url.host() == host
        && url.userInfo().isEmpty() && (url.port() == -1 || url.port() == 443) && !url.hasFragment();
}
bool safeDirectory(const QString &path) {
    if (!WorkspacePaths::isInside(path)) return false;
    QFileInfo file(path);
    while (!file.filePath().isEmpty()) {
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file.absoluteFilePath()).utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (file.isSymLink() || file.isJunction()) return false;
        const QString parent = file.absolutePath();
        if (parent == file.filePath()) break;
        file.setFile(parent);
    }
    return true;
}
QString value(const QJsonObject &metadata, const QString &key) {
    return QTextDocumentFragment::fromHtml(metadata[key].toObject()["value"].toString()).toPlainText().trimmed();
}
bool permittedLicense(const QJsonObject &metadata, QString &name, QString &url) {
    name = value(metadata, QStringLiteral("LicenseShortName"));
    url = value(metadata, QStringLiteral("LicenseUrl"));
    if (name == QStringLiteral("Public domain") && value(metadata, QStringLiteral("Copyrighted")).compare(QStringLiteral("False"), Qt::CaseInsensitive) == 0) return true;
    QUrl license(url);
    if ((license.scheme() != QStringLiteral("https") && license.scheme() != QStringLiteral("http"))
        || (license.host() != QStringLiteral("creativecommons.org") && license.host() != QStringLiteral("www.creativecommons.org"))
        || !license.userInfo().isEmpty() || license.hasQuery() || license.hasFragment()) return false;
    if (name == QStringLiteral("CC0") && license.path() == QStringLiteral("/publicdomain/zero/1.0/")) return true;
    static const QRegularExpression pattern(QStringLiteral("^/licenses/(by|by-sa)/(1\\.0|2\\.0|2\\.5|3\\.0|4\\.0)/?$"));
    const auto match = pattern.match(license.path());
    if (!match.hasMatch() || value(metadata, QStringLiteral("Artist")).isEmpty()) return false;
    const QString expected = (match.captured(1) == QStringLiteral("by") ? QStringLiteral("CC BY ") : QStringLiteral("CC BY-SA ")) + match.captured(2);
    return name == expected;
}
}

CommonsClient::CommonsClient(const QString &outputDirectory, QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_directory(QDir::cleanPath(QDir::fromNativeSeparators(outputDirectory))),
      m_network(network ? network : new QNetworkAccessManager(this)) {}
CommonsClient::~CommonsClient() { clearRequest(); }

void CommonsClient::get(const QUrl &url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "LocalCut/1.0 (Kdenlive-based desktop editor; https://kdenlive.org)");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(45000);
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(1024 * 1024);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &CommonsClient::consume);
    connect(m_reply, &QIODevice::readyRead, this, &CommonsClient::consume);
    connect(m_reply, &QNetworkReply::finished, this, &CommonsClient::completed);
}

bool CommonsClient::search(const QString &query) {
    if (m_reply) { Q_EMIT failed(QStringLiteral("busy")); return false; }
    m_assets.clear();
    if (query.trimmed().isEmpty() || query.size() > 200) { Q_EMIT failed(QStringLiteral("invalid_query")); return false; }
    QUrl url(QStringLiteral("https://commons.wikimedia.org/w/api.php"));
    QUrlQuery parameters;
    for (const auto &entry : {std::pair{"action", "query"}, {"format", "json"}, {"formatversion", "2"}, {"generator", "search"},
        {"gsrnamespace", "6"}, {"gsrlimit", "10"}, {"prop", "imageinfo"}, {"iiprop", "url|extmetadata|size|mime"}, {"maxlag", "5"}})
        parameters.addQueryItem(QString::fromLatin1(entry.first), QString::fromLatin1(entry.second));
    parameters.addQueryItem(QStringLiteral("gsrsearch"), query.trimmed());
    url.setQuery(parameters);
    m_downloading = false; m_received = 0; m_body.clear();
    get(url);
    return true;
}

bool CommonsClient::download(const QString &assetId) {
    if (m_reply) { Q_EMIT failed(QStringLiteral("busy")); return false; }
    if (!m_assets.contains(assetId)) { Q_EMIT failed(QStringLiteral("unknown_asset")); return false; }
    if (!safeDirectory(m_directory) || !QDir().mkpath(m_directory)) { Q_EMIT failed(QStringLiteral("unsafe_output_directory")); return false; }
    m_partial = std::make_unique<QTemporaryFile>(m_directory + QStringLiteral("/.commons-part-XXXXXX"));
    if (!m_partial->open()) { fail(QStringLiteral("write_failed")); return false; }
    m_metadata = m_assets[assetId]; m_downloading = true; m_received = 0; m_hash.reset();
    get(QUrl(m_metadata["originalUrl"].toString()));
    return true;
}

void CommonsClient::consume() {
    if (!m_reply) return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl().isValid()) { fail(QStringLiteral("redirect_rejected")); return; }
    if (status && status != 200) { fail(QStringLiteral("http_error")); return; }
    const qint64 limit = m_downloading ? maximumImageSize : 2LL * 1024 * 1024;
    if (m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit) { fail(QStringLiteral("response_too_large")); return; }
    if (m_downloading && status == 200) {
        const QString mime = m_reply->header(QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        if (mime != m_metadata["mimeType"].toString()) { fail(QStringLiteral("invalid_image_mime")); return; }
    }
    while (m_reply && m_reply->bytesAvailable()) {
        const QByteArray data = m_reply->read(1024 * 1024);
        if (data.isEmpty()) break;
        m_received += data.size();
        if (m_received > limit) { fail(QStringLiteral("response_too_large")); return; }
        if (m_downloading) {
            if (m_partial->write(data) != data.size()) { fail(QStringLiteral("write_failed")); return; }
            m_hash.addData(data);
        } else m_body.append(data);
    }
}

void CommonsClient::completed() {
    consume();
    if (!m_reply) return;
    if (m_reply->error() != QNetworkReply::NoError) { fail(QStringLiteral("network_error")); return; }
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) { fail(QStringLiteral("http_error")); return; }
    if (m_downloading) {
        const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (length > 0 && length != m_received) { fail(QStringLiteral("network_error")); return; }
        if (!m_partial->flush()) { fail(QStringLiteral("write_failed")); return; }
        m_partial->close();
        const QString mime = m_metadata["mimeType"].toString();
        bool validImage;
        {
            QImageReader reader(m_partial->fileName());
            validImage = QString::fromLatin1(reader.format()) == mime.section(QLatin1Char('/'), 1) && reader.size().isValid();
        }
        if (!validImage) { fail(QStringLiteral("invalid_image")); return; }
        if (!safeDirectory(m_directory)) { fail(QStringLiteral("unsafe_output_directory")); return; }
        const QString path = QDir(m_directory).filePath(QStringLiteral("commons-%1-%2.%3").arg(m_metadata["assetId"].toString(),
            QUuid::createUuid().toString(QUuid::WithoutBraces), extensions[mime]));
        if (!m_partial->rename(path)) { fail(QStringLiteral("write_failed")); return; }
        m_partial->setAutoRemove(false);
        auto metadata = m_metadata;
        metadata.insert(QStringLiteral("localFile"), path);
        metadata.insert(QStringLiteral("sha256"), QString::fromLatin1(m_hash.result().toHex()));
        metadata.insert(QStringLiteral("retrievedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        QSaveFile sidecar(path + QStringLiteral(".json"));
        const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
        if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(json) != json.size() || !sidecar.commit()) {
            QFile::remove(path); fail(QStringLiteral("metadata_write_failed")); return;
        }
        clearRequest();
        Q_EMIT downloaded(path, metadata);
        return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(m_body, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() || document.object().contains("error")) { fail(QStringLiteral("invalid_response")); return; }
    const auto query = document.object()["query"].toObject();
    if (!query.isEmpty() && !query["pages"].isArray()) { fail(QStringLiteral("invalid_response")); return; }
    QJsonArray assets;
    for (const auto &item : query["pages"].toArray()) {
        const auto page = item.toObject();
        const auto imageInfo = page["imageinfo"].toArray();
        if (imageInfo.isEmpty()) continue;
        const auto info = imageInfo.first().toObject();
        const QString mime = info["mime"].toString();
        const QUrl original(info["url"].toString()), source(info["descriptionurl"].toString());
        QString license, licenseUrl;
        if (!extensions.contains(mime) || !secureUrl(original, QStringLiteral("upload.wikimedia.org"))
            || !secureUrl(source, QStringLiteral("commons.wikimedia.org")) || !source.path().startsWith(QStringLiteral("/wiki/File:"))
            || info["size"].toInteger() <= 0 || info["size"].toInteger() > maximumImageSize
            || !permittedLicense(info["extmetadata"].toObject(), license, licenseUrl)) continue;
        const QString id = QString::number(page["pageid"].toInteger());
        if (id == QStringLiteral("0")) continue;
        const auto ext = info["extmetadata"].toObject();
        QJsonObject metadata{{"assetId", id}, {"title", page["title"].toString()}, {"sourceUrl", source.toString()},
            {"licenseName", license}, {"licenseUrl", licenseUrl}, {"author", value(ext, QStringLiteral("Artist"))},
            {"credit", value(ext, QStringLiteral("Credit"))}, {"width", info["width"]}, {"height", info["height"]},
            {"fileSize", info["size"]}, {"mimeType", mime}, {"originalUrl", original.toString()}};
        m_assets.insert(id, metadata);
        auto published = metadata; published.remove(QStringLiteral("originalUrl")); assets.append(published);
    }
    clearRequest();
    Q_EMIT searchFinished(assets);
}

void CommonsClient::clearRequest() {
    if (m_reply) {
        auto *reply = m_reply.data(); m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        if (!reply->isFinished()) reply->abort();
        reply->deleteLater();
    }
    m_partial.reset(); m_body.clear(); m_metadata = {}; m_received = 0;
}
void CommonsClient::fail(const QString &code) { clearRequest(); Q_EMIT failed(code); }
void CommonsClient::cancel() {
    if (!m_reply) return;
    clearRequest(); Q_EMIT canceled();
}
