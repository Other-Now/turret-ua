// Bridge between the SIP stack (on its own EventLoop thread) and QML (on the
// GUI thread).
//
//   QML button -> Q_INVOKABLE (GUI thread) -> loop.post(...)         -> UA
//   UA event   -> on_call_event (loop thread) -> invokeMethod(Queued) -> model row update
//
// Nothing in this class touches the UA directly from the GUI thread, and the
// UA never touches a QObject from the loop thread, so the UI cannot block call
// handling and call handling cannot block the UI.
#pragma once
#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>
#include <thread>

#include "core/event_loop.hpp"
#include "ua/user_agent.hpp"

namespace core { class UdpTransport; }
namespace rtp { class MediaManager; }

class CallModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString localUri READ localUri NOTIFY localUriChanged)
    Q_PROPERTY(int activeCalls READ activeCalls NOTIFY activeCallsChanged)

public:
    enum Roles { CallIdRole = Qt::UserRole + 1, PeerRole, StateRole, IncomingRole, RemoteHoldRole, ReasonRole };

    explicit CallModel(QObject* parent = nullptr);
    ~CallModel() override;

    // Starts the loop thread and binds the SIP socket. media=false for tests.
    bool start(const QString& ip, quint16 port, const QString& user, bool auto_answer, bool media);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString localUri() const { return local_uri_; }
    int activeCalls() const;

    Q_INVOKABLE void dial(const QString& uri);
    Q_INVOKABLE void answer(int callId);
    Q_INVOKABLE void hangup(int callId);
    Q_INVOKABLE void hold(int callId);
    Q_INVOKABLE void resume(int callId);
    Q_INVOKABLE void transfer(int callId, const QString& target);
    Q_INVOKABLE QString stateOf(int callId) const;

signals:
    void localUriChanged();
    void activeCallsChanged();
    void callChanged(int callId, const QString& state);

private:
    void apply(const ua::CallInfo& c);  // GUI thread
    template <class F> void on_loop(F f);

    struct Row {
        int id;
        QString peer, state, reason;
        bool incoming, remote_hold;
    };
    QVector<Row> rows_;
    QString local_uri_;

    core::EventLoop loop_;
    std::thread thread_;
    std::unique_ptr<core::UdpTransport> tp_;   // loop thread owns these three
    std::unique_ptr<ua::UserAgent> ua_;
    std::unique_ptr<rtp::MediaManager> media_;
};
