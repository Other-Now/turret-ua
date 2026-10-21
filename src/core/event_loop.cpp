#include "core/event_loop.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace core {

EventLoop::EventLoop() : owner_(std::this_thread::get_id()) {
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) { std::perror("eventfd"); std::abort(); }
}

EventLoop::~EventLoop() { ::close(wake_fd_); }

void EventLoop::assert_loop() const {
    if (!in_loop_thread()) {
        std::fprintf(stderr, "EventLoop: called from a foreign thread\n");
        std::abort();
    }
}

void EventLoop::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        posted_.push_back(std::move(fn));
    }
    uint64_t one = 1;
    [[maybe_unused]] auto n = ::write(wake_fd_, &one, sizeof one);
}

void EventLoop::stop() {
    stop_.store(true);
    uint64_t one = 1;
    [[maybe_unused]] auto n = ::write(wake_fd_, &one, sizeof one);
}

void EventLoop::add_fd(int fd, std::function<void()> cb) {
    assert_loop();
    fds_.push_back({fd, std::move(cb)});
}

void EventLoop::remove_fd(int fd) {
    assert_loop();
    for (auto it = fds_.begin(); it != fds_.end(); ++it)
        if (it->fd == fd) { fds_.erase(it); return; }
}

uint64_t EventLoop::now_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

TimerId EventLoop::schedule(uint64_t delay_ms, std::function<void()> fn) {
    assert_loop();
    TimerId id = ++next_timer_;
    uint64_t at = now_ms() + delay_ms;
    timers_.emplace(Key{at, id}, std::move(fn));
    timer_at_[id] = at;
    return id;
}

void EventLoop::cancel(TimerId id) {
    assert_loop();
    auto it = timer_at_.find(id);
    if (it == timer_at_.end()) return;
    timers_.erase(Key{it->second, id});
    timer_at_.erase(it);
}

void EventLoop::run() {
    owner_.store(std::this_thread::get_id());
    std::vector<pollfd> pfds;
    while (!stop_.load()) {
        int timeout = -1;
        if (!timers_.empty()) {
            uint64_t now = now_ms(), at = timers_.begin()->first.at;
            timeout = at <= now ? 0 : static_cast<int>(std::min<uint64_t>(at - now, 1000));
        }
        pfds.clear();
        pfds.push_back({wake_fd_, POLLIN, 0});
        for (auto& f : fds_) pfds.push_back({f.fd, POLLIN, 0});
        int rc = ::poll(pfds.data(), pfds.size(), timeout);
        if (rc < 0) continue;  // EINTR

        // Sockets. A handler may close another socket (hangup closes the RTP
        // session), so look each ready fd up again right before dispatching it.
        for (size_t i = 1; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & (POLLIN | POLLERR))) continue;
            std::function<void()> cb;
            for (auto& f : fds_)
                if (f.fd == pfds[i].fd) cb = f.cb;
            if (!cb) continue;
            cb();
            handlers_run_.fetch_add(1, std::memory_order_relaxed);
        }

        // Cross-thread posts.
        if (pfds[0].revents & POLLIN) {
            uint64_t v;
            [[maybe_unused]] auto n = ::read(wake_fd_, &v, sizeof v);
        }
        std::vector<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lk(mu_);
            work.swap(posted_);
        }
        for (auto& fn : work) { fn(); handlers_run_.fetch_add(1, std::memory_order_relaxed); }

        // Due timers, in deadline order. A timer may schedule more timers.
        uint64_t now = now_ms();
        while (!timers_.empty() && timers_.begin()->first.at <= now) {
            auto it = timers_.begin();
            auto fn = std::move(it->second);
            timer_at_.erase(it->first.id);
            timers_.erase(it);
            fn();
            handlers_run_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Drain posts made before stop() so a final post (e.g. a hangup) is not lost.
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lk(mu_);
        work.swap(posted_);
    }
    for (auto& fn : work) fn();
}

}  // namespace core
