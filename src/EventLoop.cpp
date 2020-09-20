#include "EventLoop.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <unistd.h>
#include <sys/eventfd.h>
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
    EventLoop::HandlerType mInterruptHandler;
    int mEpollFd;
    int mInterruptFd;

    EventLoopInternal() {
        mEpollFd = epoll_create1(0);
        if (mEpollFd == -1) {
            throw std::runtime_error("failed to create epoll file descriptor");
        }

        mInterruptFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (mInterruptFd == -1) {
            throw std::runtime_error("failed to create eventfd");
        }

        mInterruptHandler = [this] (uint32_t events) {
            close(mEpollFd);
            close(mInterruptFd);
            mEpollFd = -1;
            mInterruptFd = -1;
        };

        struct epoll_event event { .events = EPOLLIN, .data = { .ptr = &mInterruptHandler } };
        if (epoll_ctl(mEpollFd, EPOLL_CTL_ADD, mInterruptFd, &event) == -1) {
            throw std::runtime_error("failed to add loop interrupt handler");
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
        if (mLoop.joinable()) {
            uint64_t data = 1;
            write(mInterruptFd, &data, sizeof data);
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

    if (mHandlers.find(fd) != mHandlers.end()) {
        removeEvent(fd);
    }

    setNonBlocking(fd);
    auto it = mHandlers.insert({fd, handler});
    struct epoll_event event { .events = events, .data = { .ptr = &it.first->second } };
    return epoll_ctl(mState->mEpollFd, EPOLL_CTL_ADD, fd, &event) != -1;
}

bool EventLoop::modifyEvent(int fd, uint32_t events) {
    // we have to be careful with state (are we a moved object?)
    decltype(mHandlers)::iterator it;
    if (!mState || (it = mHandlers.find(fd)) == mHandlers.end()) {
        return false;
    }

    struct epoll_event event { .events = events, .data = { .ptr = &it->second } };
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
