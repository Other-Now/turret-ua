// Single-threaded reactor: poll() over sockets + a timer heap + a cross-thread
// post() queue (eventfd wake-up). Everything that touches SIP or call state runs
// here, so the UA core never needs a lock. Other threads (Qt UI, tests) talk to
// it only through post().
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/interfaces.hpp"

namespace core {

class EventLoop : public Timers {
public:
    EventLoop();
    ~EventLoop() override;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Thread-safe.
    void post(std::function<void()> fn);
    void stop();

    // Loop thread only (asserted).
    void add_fd(int fd, std::function<void()> on_readable);
    void remove_fd(int fd);
    uint64_t now_ms() const override;
    TimerId schedule(uint64_t delay_ms, std::function<void()> fn) override;
    void cancel(TimerId id) override;

    // Runs on the calling thread until stop(). The calling thread becomes the loop thread.
    void run();
    bool in_loop_thread() const { return std::this_thread::get_id() == owner_.load(); }

    uint64_t handlers_run() const { return handlers_run_.load(std::memory_order_relaxed); }

private:
    void assert_loop() const;
    struct Key {
        uint64_t at;
        TimerId id;
        bool operator<(const Key& o) const { return at < o.at || (at == o.at && id < o.id); }
    };

    int wake_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<std::thread::id> owner_;
    std::mutex mu_;
    std::vector<std::function<void()>> posted_;
    std::map<Key, std::function<void()>> timers_;
    std::map<TimerId, uint64_t> timer_at_;
    TimerId next_timer_ = 0;
    struct Fd { int fd; std::function<void()> cb; };
    std::vector<Fd> fds_;
    std::atomic<uint64_t> handlers_run_{0};
};

}  // namespace core
