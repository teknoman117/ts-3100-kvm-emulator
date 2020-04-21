#ifndef EVENTLOOP_H_
#define EVENTLOOP_H_

#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

#include <sys/epoll.h>

struct EventLoopInternal;

class EventLoop
{
public:
    using HandlerType = std::function<void(uint32_t)>;

private:
    std::shared_ptr<EventLoopInternal> mState;
    std::unordered_map<int, HandlerType> mHandlers;
    void removeHandlers();

public:
    EventLoop();
    EventLoop(const EventLoop&);
    EventLoop(EventLoop&& loop);
    ~EventLoop();

    EventLoop& operator=(const EventLoop&);
    EventLoop& operator=(EventLoop&&);

    bool addEvent(int fd, uint32_t events, HandlerType handler);
    bool modifyEvent(int fd, uint32_t events);
    void removeEvent(int fd);
};

#endif /* EVENTLOOP_H_ */