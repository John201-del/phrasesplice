// A real child process which deliberately never reads its stdin pipe.
#include <QCoreApplication>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTimer::singleShot(30000, &app, &QCoreApplication::quit);
    return app.exec();
}
