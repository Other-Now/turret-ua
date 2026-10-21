// Qt-side tests, run offscreen: two CallModels (each with its own SIP loop
// thread) call each other in-process; the model rows must follow the call, and
// Main.qml must load cleanly against the model.
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

#include "call_model.hpp"

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void callFlowUpdatesModel() {
        CallModel alice, bob;
        QVERIFY(alice.start("127.0.0.1", 0, "alice", false, false));
        QVERIFY(bob.start("127.0.0.1", 0, "bob", true, false));
        alice.dial(bob.localUri());
        QTRY_COMPARE_WITH_TIMEOUT(alice.rowCount(), 1, 3000);
        const int id = alice.data(alice.index(0), CallModel::CallIdRole).toInt();
        QTRY_COMPARE_WITH_TIMEOUT(alice.stateOf(id), QString("Connected"), 3000);
        QCOMPARE(bob.rowCount(), 1);
        QCOMPARE(bob.data(bob.index(0), CallModel::IncomingRole).toBool(), true);

        alice.hold(id);
        QTRY_COMPARE_WITH_TIMEOUT(alice.stateOf(id), QString("OnHold"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(bob.data(bob.index(0), CallModel::RemoteHoldRole).toBool(), 3000);

        alice.resume(id);
        QTRY_COMPARE_WITH_TIMEOUT(alice.stateOf(id), QString("Connected"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!bob.data(bob.index(0), CallModel::RemoteHoldRole).toBool(), 3000);

        alice.hangup(id);
        QTRY_COMPARE_WITH_TIMEOUT(bob.data(bob.index(0), CallModel::StateRole).toString(), QString("Terminated"), 3000);
        QCOMPARE(alice.activeCalls(), 0);
    }

    void qmlLoadsAndRendersCalls() {
        CallModel alice, bob;
        QVERIFY(alice.start("127.0.0.1", 0, "alice", false, false));
        QVERIFY(bob.start("127.0.0.1", 0, "bob", true, false));
        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& w) { warnings += w; });
        engine.rootContext()->setContextProperty("phone", &alice);
        engine.load(QUrl(QStringLiteral("qrc:/TurretUa/apps/gui/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(win);
        auto* list = win->findChild<QObject*>("callList");
        QVERIFY(list);

        alice.dial(bob.localUri());
        QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(alice.stateOf(1), QString("Connected"), 3000);
        alice.hold(1);
        QTRY_COMPARE_WITH_TIMEOUT(alice.stateOf(1), QString("OnHold"), 3000);
        for (const auto& w : warnings) qWarning() << w.toString();
        QVERIFY(warnings.isEmpty());

        if (qEnvironmentVariableIsSet("TU_SCREENSHOT")) {
            QTest::qWait(200);
            QVERIFY(win->grabWindow().save(qEnvironmentVariable("TU_SCREENSHOT")));
        }
        alice.hangup(1);
        QTRY_COMPARE_WITH_TIMEOUT(alice.activeCalls(), 0, 3000);
    }
};

QTEST_MAIN(GuiTest)
#include "test_gui.moc"
