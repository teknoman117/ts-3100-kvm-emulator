#include "EventLoop.hpp"

#include <algorithm>
#include <array>

#include <unistd.h>
#include <sys/fcntl.h>

namespace
{
    inline bool setNonBlocking(int fd)
    {
        int ret = fcntl(fd, F_GETFL, 0);
        if (ret == -1) {
            return false;
        }

        return fcntl(fd, F_SETFL, ret | O_NONBLOCK) != -1;
    }
} /* anonymous */

// internal event loop event-invoker
struct EventLoopInternal {
    std::thread mLoop;
    int mEpollFd;

    EventLoopInternal() {
        mEpollFd = epoll_create1(0);
        if (mEpollFd == -1) {
            throw std::runtime_error("failed to create epoll file descriptor");
        }

        mLoop = std::thread([this] () {
            std::array<struct epoll_event, 64> events;
            int n;
            while (mEpollFd != -1
                    && (n = epoll_wait(mEpollFd, events.data(), events.size(), -1) != -1)) {
                // dispatch handlers for all events
                std::for_each(events.begin(), events.begin() + n, [&] (struct epoll_event event) {
                    (*reinterpret_cast<EventLoop::HandlerType*>(event.data.ptr))(event.events);
                });
            }
        });
    }
    
    ~EventLoopInternal() {
        if (mEpollFd != -1) {
            close(mEpollFd);
            mEpollFd = -1;
        }
        if (mLoop.joinable()) {
            mLoop.join();
        }
    }

    // non-copyable, non-movable
    EventLoopInternal(const EventLoopInternal&) = delete;
    EventLoopInternal(EventLoopInternal&& loop) = delete;
    EventLoopInternal& operator=(const EventLoopInternal&) = delete;
    EventLoopInternal& operator=(EventLoopInternal&&) = delete;
};

// helper to drain all handlers
void EventLoop::removeHandlers()
{
    if (mState) {
        for (auto handlerEntry : mHandlers) {
            epoll_ctl(mState->mEpollFd, EPOLL_CTL_DEL, handlerEntry.first, nullptr);
        }
        mHandlers.clear();
    }
}

// base constructor creates a new event loop
EventLoop::EventLoop() : mState(new EventLoopInternal()), mHandlers{} {}

// copy constructor subscribes to the internal state, doesn't copy the handlers
EventLoop::EventLoop(const EventLoop& loop) : mState(loop.mState), mHandlers{} {}

// move constructor takes the other loop's handlers
EventLoop::EventLoop(EventLoop&& loop)
    : mState{std::move(loop.mState)}, mHandlers{std::move(loop.mHandlers)} {}

// destructor removes all of our handlers from the state
EventLoop::~EventLoop()
{
    removeHandlers();
}

// assignment operator deletes our handlers and sets our state to the remote state object
EventLoop& EventLoop::operator=(const EventLoop& loop)
{
    removeHandlers();
    mState = loop.mState;
    return *this;
}

// move operator deletes our handlers and takes the remote state object
EventLoop& EventLoop::operator=(EventLoop&& loop) {
    removeHandlers();
    mState = std::move(loop.mState);
    mHandlers = std::move(loop.mHandlers);
    return *this;
}

bool EventLoop::addEvent(int fd, uint32_t events, EventLoop::HandlerType handler)
{
    // we have to be careful with state (are we a moved object?)
    if (!mState) {
        return false;
    }

    // TODO: something reasonable when we try to add a duplicate descriptor (overwrite the old?)
    if (mHandlers.find(fd) != mHandlers.end()) {
        return false;
    }

    auto it = mHandlers.insert({fd, handler});
    struct epoll_event event{};
    event.events = events;
    event.data.ptr = &it.first->second;
    setNonBlocking(fd);
    return epoll_ctl(mState->mEpollFd, EPOLL_CTL_ADD, fd, &event) != -1;
}

bool EventLoop::modifyEvent(int fd, uint32_t events) {
    // we have to be careful with state (are we a moved object?)
    decltype(mHandlers)::iterator it;
    if (!mState || (it = mHandlers.find(fd)) == mHandlers.end()) {
        return false;
    }

    struct epoll_event event{};
    event.events = events;
    event.data.ptr = &it->second;
    return epoll_ctl(mState->mEpollFd, EPOLL_CTL_MOD, fd, &event) != -1;
}

void EventLoop::removeEvent(int fd) {
    // we have to be careful with state (are we a moved object?)
    decltype(mHandlers)::iterator it;
    if (!mState || (it = mHandlers.find(fd)) == mHandlers.end()) {
        return;
    }

    if (epoll_ctl(mState->mEpollFd, EPOLL_CTL_DEL, fd, nullptr) != -1) {
        mHandlers.erase(it);
    }
}