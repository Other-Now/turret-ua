#include "call_model.hpp"

#include <QMetaObject>
#include <future>

#include "core/udp_transport.hpp"
#include "rtp/media.hpp"

CallModel::CallModel(QObject* parent) : QAbstractListModel(parent) {}

CallModel::~CallModel() {
    if (!thread_.joinable()) return;
    // Tear the stack down on its own thread, then stop the loop.
    std::promise<void> done;
    loop_.post([&] {
        ua_.reset();
        media_.reset();
        tp_.reset();
        done.set_value();
    });
    done.get_future().wait();
    loop_.stop();
    thread_.join();
}

template <class F>
void CallModel::on_loop(F f) {
    loop_.post(std::move(f));
}

bool CallModel::start(const QString& ip, quint16 port, const QString& user, bool auto_answer, bool media) {
    thread_ = std::thread([this] { loop_.run(); });
    std::promise<QString> bound;
    const std::string sip_ip = ip.toStdString(), name = user.toStdString();
    loop_.post([&, sip_ip, name] {
        tp_ = std::make_unique<core::UdpTransport>(loop_, sip_ip, port);
        if (!tp_->ok()) { bound.set_value({}); return; }
        ua::Config cfg;
        cfg.user = name;
        cfg.auto_answer = auto_answer;
        cfg.answer_delay_ms = auto_answer ? 300 : 0;
        ua_ = std::make_unique<ua::UserAgent>(*tp_, loop_, cfg);
        tp_->on_datagram = [this](const std::string& d, const core::Endpoint& s) { ua_->on_datagram(d, s); };
        if (media) {
            media_ = std::make_unique<rtp::MediaManager>(loop_, sip_ip, static_cast<uint16_t>(tp_->local().port + 1000), 60);
            ua_->set_media(media_.get());
        }
        // Loop thread -> GUI thread. CallInfo is copied into the queued functor.
        ua_->on_call_event = [this](const ua::CallInfo& c) {
            QMetaObject::invokeMethod(this, [this, c] { apply(c); }, Qt::QueuedConnection);
        };
        bound.set_value(QString::fromStdString("sip:" + name + "@" + tp_->local().str()));
    });
    local_uri_ = bound.get_future().get();
    emit localUriChanged();
    return !local_uri_.isEmpty();
}

void CallModel::apply(const ua::CallInfo& c) {
    Row r{c.id, QString::fromStdString(c.peer), QString::fromLatin1(ua::to_string(c.state)),
          QString::fromStdString(c.reason), c.dir == ua::Direction::Incoming, c.remote_hold};
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].id != c.id) continue;
        rows_[i] = r;
        emit dataChanged(index(i), index(i));
        emit callChanged(c.id, r.state);
        emit activeCallsChanged();
        return;
    }
    beginInsertRows({}, rows_.size(), rows_.size());
    rows_.push_back(r);
    endInsertRows();
    emit callChanged(c.id, r.state);
    emit activeCallsChanged();
}

int CallModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant CallModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const Row& r = rows_[index.row()];
    switch (role) {
        case CallIdRole: return r.id;
        case PeerRole: return r.peer;
        case StateRole: return r.state;
        case IncomingRole: return r.incoming;
        case RemoteHoldRole: return r.remote_hold;
        case ReasonRole: return r.reason;
        default: return {};
    }
}

QHash<int, QByteArray> CallModel::roleNames() const {
    return {{CallIdRole, "callId"},     {PeerRole, "peer"},           {StateRole, "callState"},
            {IncomingRole, "incoming"}, {RemoteHoldRole, "remoteHold"}, {ReasonRole, "reason"}};
}

int CallModel::activeCalls() const {
    int n = 0;
    for (const auto& r : rows_)
        if (r.state != QLatin1String("Terminated")) ++n;
    return n;
}

QString CallModel::stateOf(int callId) const {
    for (const auto& r : rows_)
        if (r.id == callId) return r.state;
    return {};
}

void CallModel::dial(const QString& uri) {
    on_loop([this, u = uri.toStdString()] { ua_->dial(u); });
}
void CallModel::answer(int id) { on_loop([this, id] { ua_->answer(id); }); }
void CallModel::hangup(int id) { on_loop([this, id] { ua_->hangup(id); }); }
void CallModel::hold(int id) { on_loop([this, id] { ua_->hold(id); }); }
void CallModel::resume(int id) { on_loop([this, id] { ua_->resume(id); }); }
void CallModel::transfer(int id, const QString& target) {
    on_loop([this, id, t = target.toStdString()] { ua_->transfer(id, t); });
}
