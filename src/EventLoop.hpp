#ifndef EVENTLOOP_H_
#define EVENTLOOP_H_

#include <functional>
#include <thread>
#include <unordered_map>

class EventLoop
{
    int mEpollFd;
    std::unordered_map<int, std::function<void(int,uint32_t)>> mHandlers;
    std::thread mLoop;

public:
    EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop(EventLoop&& loop);

    virtual ~EventLoop();

    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop& operator=(EventLoop&&);
};

#endif /* EVENTLOOP_H_ */