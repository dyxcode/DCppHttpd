#ifndef HEARTEN_EVENT_H_
#define HEARTEN_EVENT_H_

#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include <functional>
#include <thread>
#include <condition_variable>

#include "log.h"

namespace hearten {

enum class IO : int {
  // for epoll event and callback
  kRead = EPOLLIN | EPOLLPRI,
  kWrite = EPOLLOUT,
  // only for callback
  kError = 1 << 8,
  kClose = 1 << 9,
  // only for epoll event
  kAll = kRead | kWrite,
  kNone = 0
};

namespace detail {

template<typename Clock>
class PeriodicTask {
  using Period = typename Clock::duration;
  using TimePoint = typename Clock::time_point;
public:
  template<typename T>
  PeriodicTask(T&& task, const Period& period, int times)
    : task_(std::forward<T>(task)), period_(period), times_(times) { }

  template<typename T, typename C, typename U>
  void operator()(T&& map_node, C& container, U& u_lock) {
    if (--times_ >= 0) {
      if (times_ != 0) {
        map_node.key() += period_;
        container.insert(std::move(map_node));
      }
      u_lock.unlock();
      task_();
      u_lock.lock();
    }
  }
  TimePoint getEndTime(const TimePoint& execute_time) const {
    return execute_time + period_ * times_;
  }

  PeriodicTask(const PeriodicTask&) = delete;
  PeriodicTask(PeriodicTask&&) = default;

private:
  std::function<void()> task_;
  Period period_;
  int times_;
};

template<typename T>
class HashListQueue {
public:
  void put(const T& key) {
    iter_to_node_[key] = nodes_.emplace(nodes_.end(), key);
  }
  T get() const {
    return nodes_.front();
  }
  void remove(const T& key) {
    nodes_.erase(iter_to_node_[key]);
    iter_to_node_.erase(key);
  }
  bool empty() const {
    return nodes_.empty();
  }
private:
  std::unordered_map<T, typename std::list<T>::iterator> iter_to_node_;
  std::list<T> nodes_;
};

template<typename Clock, size_t ThreadNumber>
class Scheduler : Noncopyable {
  using TimePoint = typename Clock::time_point;
  using Duration = typename Clock::duration;
  using PeriodicTask = PeriodicTask<Clock>;
  using Iter = typename std::multimap<TimePoint, PeriodicTask>::iterator;
public:
  Scheduler()
    : stop_(false), delay_wake_up_thread_(0), cvs_(ThreadNumber) {
    for (size_t i = 1; i <= ThreadNumber; ++i) {
      threads_.emplace_back([i, this]{
        std::unique_lock<std::mutex> u_lock{mtx_};
        while (true) {
          if (stop_) break;
          if (tasks_.empty() || delay_wake_up_thread_ != threads_.size()) {
            orders_.put(i);
            cvs_[i].wait(u_lock);
            orders_.remove(i);
          } else {
            auto && execute_time = tasks_.begin()->first;
            if (execute_time <= Clock::now()) {
              auto map_node = tasks_.extract(tasks_.begin());
              if (!tasks_.empty() && !orders_.empty())
                cvs_[orders_.get()].notify_one();
              auto& task = map_node.mapped();
              task(map_node, tasks_, u_lock);
            } else {
              orders_.put(i);
              delay_wake_up_thread_ = i;
              cvs_[i].wait_until(u_lock, execute_time);
              delay_wake_up_thread_ = threads_.size();
              orders_.remove(i);
            }
          }
        }
      });
    }
  }
  ~Scheduler() {
    {
      std::lock_guard<std::mutex> guard{mtx_};
      stop_ = true;
    }
    for (auto && item : cvs_)
      item.notify_one();
    for (auto && item : threads_)
      item.join();
  }

  template<typename F>
  auto execute(F&& task,
               const TimePoint& execute_time,
               const Duration& period = Duration::zero(),
               int times = 1) {
    auto end_time = task.getEndTime(execute_time);
    size_t index;
    {
      std::lock_guard<std::mutex> guard{mtx_};
      auto iter = tasks_.emplace(execute_time, std::move(task));
      if (orders_.empty())
        index = cvs_.size();
      else if (delay_wake_up_thread_ == cvs_.size())
        index = orders_.get();
      else
        index = delay_wake_up_thread_;
    }
    if (index != cvs_.size())
      cvs_[index].notify_one();
    return [callable = true, end_time, iter, this] () mutable {
      if (callable == false) return;
      std::unique_lock<std::mutex> u_lock{mtx_};
      if (end_time < Clock::now()) return;
      size_t index = delay_wake_up_thread_;
      bool need_notify = (iter == tasks_.begin())
                        && (index != cvs_.size());
      tasks_.erase(iter);
      u_lock.unlock();
      if (need_notify)
        cvs_[index].notify_one();
      callable = false;

    };
  }

private:
  bool stop_;
  size_t delay_wake_up_thread_;
  std::mutex mtx_;
  std::vector<std::condition_variable> cvs_;
  std::vector<std::thread> threads_;
  std::multimap<TimePoint, PeriodicTask> tasks_;
  HashListQueue<size_t> orders_;
};

template<typename Clock>
class TimerHandle : Noncopyable {
  using TimePoint = typename Clock::time_point;
  using Duration = typename Clock::duration;
public:
  TimerHandle(std::function<void()> task,
      const TimePoint& execute_time,
      const Duration& period,
      int times)
  void cancel() {
  }
  void reset();

private:
  std::mutex& mtx_;
  std::multimap<TimePoint, PeriodicTask<Clock>>& tasks_;
};

class Channel : Noncopyable {
public:
  Channel() : events_(0) { }

  void handleEvent(int revents) {
    if ((revents & EPOLLHUP) && !(revents & EPOLLIN) && close_cb_)
      close_cb_();
    if ((revents & (EPOLLERR)) && error_cb_)
      error_cb_();
    if ((revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) && read_cb_)
      read_cb_();
    if ((revents & EPOLLOUT) && write_cb_)
      write_cb_();
  }

  Channel& setReadCallback(std::function<void()> cb)
  { read_cb_  = std::move(cb); return *this; }
  Channel& setWriteCallback(std::function<void()> cb)
  { write_cb_ = std::move(cb); return *this; }
  Channel& setErrorCallback(std::function<void()> cb)
  { error_cb_ = std::move(cb); return *this; }
  Channel& setCloseCallback(std::function<void()> cb)
  { close_cb_ = std::move(cb); return *this; }

  int getEvents() const { return events_; }
  void setEvents(int flag) { events_ |= flag; }
  void unsetEvents(int flag) { events_ &= ~flag; }
  bool checkEvents(int flag) {
    if (flag == 0) return events_ == 0;
    return (events_ & flag) == flag;
  }

private:
  int events_;
  std::function<void()> read_cb_;
  std::function<void()> write_cb_;
  std::function<void()> error_cb_;
  std::function<void()> close_cb_;
};

template<size_t EventListSize>
class Epoller : Noncopyable {
public:
  Epoller()
    : epfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(EventListSize) {
    ASSERT(epfd_ != -1);
  }
  ~Epoller() {
    ASSERT(::close(epfd_) != -1);
  }

  void monitor(int fd, Channel& channel) {
    auto iter = fds_.find(fd);
    if (iter == fds_.end()) {
      // check is none event
      if (channel.checkEvents(0)) return;
      fds_.emplace(fd);
      updateEpollEvent(EPOLL_CTL_ADD, fd, channel);
    } else {
      // existed channel
      if (channel.checkEvents(0)) {
        fds_.emplace(iter);
        updateEpollEvent(EPOLL_CTL_DEL, fd, channel);
      } else {
        updateEpollEvent(EPOLL_CTL_MOD, fd, channel);
      }
    }
  }

  void setTimeout(int timeout) {
    // get the number of ready events
    int ready_events_number = ::epoll_wait(epfd_, events_.data(),
        static_cast<int>(events_.size()), timeout);
    ASSERT(ready_events_number != -1);

    // really have events to deal with
    if (size_t n = ready_events_number; n > 0) {
      ASSERT(n <= events_.size());
      // execute events callback
      for (int i = 0; i < n; ++i) {
        auto channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->handleEvent(events_[i].events);
      }
      // resizing the eventlist
      if (n == events_.size())
        events_.resize(events_.size() * 2);
      else if (n <= events_.size() / 2 && events_.size() > EventListSize)
        events_.resize(events_.size() / 2);
    }
  }

private:
  void updateEpollEvent(int operation, int fd, Channel& channel) {
    epoll_event ev;
    ::memset(&ev, 0, sizeof(ev));
    ev.events = channel.getEvents();
    ev.data.ptr = &channel;
    ASSERT(::epoll_ctl(epfd_, operation, fd, &ev) == 0);
  }

  int epfd_;
  std::unordered_set<int> fds_;
  std::vector<epoll_event> events_;
};

template<size_t EventListSize>
class IOHandle : Noncopyable {
  using Epoller = Epoller<EventListSize>;
public:
  IOHandle(int fd, Epoller& epoller) : fd_(fd), epoller_(epoller) { }

  void open(IO flag) {
    if (isEpollEvent(flag)) {
      channel_.setEvents(static_cast<int>(flag));
      epoller_.monitor(fd_, channel_);
    }
  }
  void close(IO flag) {
    if (isEpollEvent(flag)) {
      channel_.unsetEvents(static_cast<int>(flag));
      epoller_.monitor(fd_, channel_);
    }
  }
  void check(IO flag) {
    if (isEpollEvent(flag))
      channel_.checkEvents(static_cast<int>(flag));
  }
  void set(IO flag, std::function<void()> cb) {
    switch (flag) {
      case IO::kRead : channel_.setReadCallback(std::move(cb)); break;
      case IO::kWrite : channel_.setWriteCallback(std::move(cb)); break;
      case IO::kError : channel_.setErrorCallback(std::move(cb)); break;
      case IO::kClose : channel_.setCloseCallback(std::move(cb)); break;
      default : break;
    }
  }

private:
  bool isEpollEvent(IO flag) {
    return !(flag == IO::kError || flag == IO::kClose);
  }

  int fd_;
  Channel channel_;
  Epoller& epoller_;
};

} // namespace detail

template<typename Clock, size_t ThreadNumber, size_t EventListSize>
class Processor : detail::Noncopyable {
  using TimePoint = typename Clock::time_point;
  using Duration = typename Clock::duration;
public:
  Processor() {}

  detail::IOHandle<EventListSize> io(int fd) {
    return detail::IOHandle<EventListSize>(fd, epoller_);
  }

  detail::TimerHandle<Clock> timer(std::function<void()> task,
          const TimePoint& execute_time = Clock::now(),
          const Duration& period = Duration::zero(),
          int times = 1) {
    return detail::TimerHandle<Clock>
      (std::move(task), execute_time, period, times);
  }

private:
  bool stop_;
  detail::Epoller<EventListSize> epoller_;
  detail::Scheduler<Clock, ThreadNumber> scheduler_;
};

} // namespace hearten

#endif
