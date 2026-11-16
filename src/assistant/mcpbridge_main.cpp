/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include <QCoreApplication>
#include <QLocalSocket>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr qint64 maximumMessage = 8 * 1024 * 1024;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 3 || arguments[1] != QLatin1String("--socket") || arguments[2].isEmpty()) {
        std::fputs("Usage: localcut-mcp --socket <PhraseSplice pipe name>\n", stderr);
        return 2;
    }
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QLocalSocket socket;
    socket.setReadBufferSize(maximumMessage + 1);
    QByteArray output;
    std::mutex outputMutex;
    std::condition_variable outputReady;
    std::deque<QByteArray> outputQueue;
    qint64 queuedBytes = 0;
    bool ending = false;
    auto fail = [&app, &ending](const char *message) {
        if (ending) return;
        ending = true;
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        app.exit(1);
    };
    QObject::connect(&socket, &QLocalSocket::errorOccurred, &app, [&](QLocalSocket::LocalSocketError) {
        fail("PhraseSplice pipe is unavailable or disconnected. Start PhraseSplice and enable WorkBuddy first.");
    });
    QObject::connect(&socket, &QLocalSocket::disconnected, &app, [&]() { fail("PhraseSplice pipe disconnected."); });
    // stdout is another potentially blocked pipe. Never write it on the socket
    // event thread: editor shutdown must still terminate an unresponsive client.
    std::thread([&]() {
        for (;;) {
            QByteArray line;
            {
                std::unique_lock<std::mutex> lock(outputMutex);
                outputReady.wait(lock, [&]() { return !outputQueue.empty(); });
                line = std::move(outputQueue.front());
                outputQueue.pop_front();
                queuedBytes -= line.size();
            }
            if (std::fwrite(line.constData(), 1, size_t(line.size()), stdout) != size_t(line.size()) || std::fflush(stdout) != 0) {
                QMetaObject::invokeMethod(&app, [&]() { fail("Unable to write MCP response to stdout."); }, Qt::QueuedConnection);
                return;
            }
        }
    }).detach();
    QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&]() {
        while (socket.bytesAvailable()) {
            output += socket.read(maximumMessage + 1 - output.size());
            qsizetype newline;
            while ((newline = output.indexOf('\n')) >= 0) {
                const auto line = output.left(newline + 1);
                output.remove(0, newline + 1);
                {
                    std::lock_guard<std::mutex> lock(outputMutex);
                    if (queuedBytes + line.size() > 2 * maximumMessage || outputQueue.size() >= 128) {
                        fail("MCP client is not reading responses; output queue is full.");
                        return;
                    }
                    queuedBytes += line.size();
                    outputQueue.push_back(line);
                }
                outputReady.notify_one();
            }
            if (output.size() > maximumMessage) {
                fail("PhraseSplice response exceeds 8 MiB.");
                return;
            }
        }
    });
    QTimer connectionTimeout;
    connectionTimeout.setSingleShot(true);
    QObject::connect(&connectionTimeout, &QTimer::timeout, &app, [&]() { fail("Timed out connecting to PhraseSplice."); });
    QObject::connect(&socket, &QLocalSocket::connected, &app, [&]() {
        connectionTimeout.stop();
        // Only one input line can be queued: BlockingQueuedConnection provides
        // backpressure while keeping stdin reads off the socket's event thread.
        std::thread([&]() {
            QByteArray line;
            for (;;) {
                const int ch = std::getc(stdin);
                if (ch == EOF) {
                    const bool readError = std::ferror(stdin);
                    QMetaObject::invokeMethod(&app, [&, readError]() {
                        ending = true;
                        socket.disconnectFromServer();
                        app.exit(readError ? 1 : 0);
                    }, Qt::QueuedConnection);
                    return;
                }
                line.append(char(ch));
                if (line.size() > maximumMessage && ch != '\n') {
                    QMetaObject::invokeMethod(&app, [&]() { fail("MCP input exceeds 8 MiB."); }, Qt::QueuedConnection);
                    return;
                }
                if (ch != '\n') continue;
                bool accepted = false;
                QMetaObject::invokeMethod(&app, [&, message = std::move(line)]() {
                    if (socket.state() == QLocalSocket::ConnectedState && socket.bytesToWrite() + message.size() <= 2 * maximumMessage) {
                        accepted = socket.write(message) == message.size();
                    }
                    if (!accepted) fail("PhraseSplice pipe cannot accept more MCP input.");
                }, Qt::BlockingQueuedConnection);
                if (!accepted) return;
                line.clear();
            }
        }).detach();
    });
    // Defer connection until exec(): immediate connection failures must set its exit code.
    QTimer::singleShot(0, &app, [&]() {
        connectionTimeout.start(10000);
        socket.connectToServer(arguments[2]);
    });
    const int result = app.exec();
    std::fflush(stderr);
    // A stdio client may leave stdin open after the editor exits. This small relay
    // owns no persistent data; process exit releases blocked stdio threads. Do not
    // flush stdout here: it may be the very pipe whose reader has stopped.
    std::_Exit(result);
}
