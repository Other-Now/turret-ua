// turret-gui: QML softphone.
//   turret-gui --bind 127.0.0.1:5060 --user alice [--auto-answer] [--call URI] [--screenshot out.png]
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

#include "call_model.hpp"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QString bind = "127.0.0.1:5060", user = "turret", call, shot;
    bool auto_answer = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        auto next = [&] { return i + 1 < args.size() ? args[++i] : QString(); };
        if (args[i] == "--bind") bind = next();
        else if (args[i] == "--user") user = next();
        else if (args[i] == "--call") call = next();
        else if (args[i] == "--screenshot") shot = next();
        else if (args[i] == "--auto-answer") auto_answer = true;
    }
    const int colon = bind.lastIndexOf(':');
    CallModel phone;
    if (!phone.start(bind.left(colon), static_cast<quint16>(bind.mid(colon + 1).toUInt()), user, auto_answer, true))
        return 1;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("phone", &phone);
    engine.load(QUrl(QStringLiteral("qrc:/TurretUa/apps/gui/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;

    if (!call.isEmpty()) QTimer::singleShot(200, &phone, [&] { phone.dial(call); });
    if (!shot.isEmpty()) {
        // Demo mode: let the call progress, grab the window, exit.
        QTimer::singleShot(2500, &app, [&] {
            auto* w = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            w->grabWindow().save(shot);
            app.quit();
        });
    }
    return app.exec();
}
