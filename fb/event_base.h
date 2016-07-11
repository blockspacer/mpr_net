#ifndef FB_EVENT_BASE_H_
#define FB_EVENT_BASE_H_

#include "fb/async_timeout.h"
#include "fb/timeout_manager.h"
#include "fb/request.h"
#include "fb/executor.h"
#include "fb/driable_executor.h"

#include <boost/intrusive/list.hpp>

#include <event.h>
#include <errno.h>
#include <math.h>

#include <atomic>
#include <memory>
#include <stack>
#include <list>
#include <queue>
#include <set>
#include <utility>
#include <functional>

namespace fb {

typedef std::function<void()> Cob;
template<typename Message> class NitificationQueue;

class EventBaseObserver {
 public:
  virtual ~EventBaseObserver() {}
  virtual uint32_t GetSampleRate() const = 0;
  virtual void LoopSample(int64_t busy_time, int64_t idle_time) = 0;
};

class RequestEventBase : public RequestData {
 public:
  static EventBase* get() {
    auto data = dynamic_cast<RequestEventBase*>(RequestContext::get()->GetContextData(kContextDataName));
    if (!data) {
      return nullptr;
    }
    return data->eb_;
  }
  static void set(EventBase* eb) {
    RequestContext::get()->SetContextData(kContextDataName,
                                          std::unique_ptr<RequestEventBase>(new RequestEventBase(eb)));
  }

 private:
  explicit RequestEventBase(EventBase* eb) : eb_(eb) {}

  EventBase* eb_;
  static constexpr const char* kContextDataName{"EventBase"};
};

//
class EventBase : public TimeoutManager,
                  public DriableExecutor {
 public:
  class LoopCallback {
   public:
    virtual ~LoopCallback() {}
    virtual void RunLoopCallback() noexcept = 0;
    void CancelLoopCallback() {
      hook_.unlink();
    } 

    bool IsLoopCallbackScheduled() const {
      return hook_.is_linked();
    }

   private:
    typedef boost::intrusive::list_member_hook<
      boost::intrusive::link_mode<boost::intrusive::auto_unlink>> ListHook;

    ListHook hook_;
    typedef boost::intrusive::list<LoopCallback,
                                   boost::intrusive::member_hook<LoopCallback,
                                                                 ListHook,
                                                                 &LoopCallback::host_>,
                                   boost::intrusive::constant_time_size<false> > List;
    friend class EventBase;
    std::shared_ptr<RequestContext> context_;
  }; // class LoopCallback
  
  // Member functions
  explicit EventBase(bool enable_time_measurement = true);
  explicit EventBase(event_base* evb, bool enable_time_measurement=true);
  ~EventBase();

  bool Loop();
  bool LoopOnce(int flags=0);
  void LoopForever();
  void TerminateLoopSoon();

  void RunInLoop(LoopCallback* callback, bool this_iteration=false);
  void RunInLoop(const Cob& c, bool this_iteration=false);
  void RunInLoop(Co&& c, bool this_iteration=false);
  void RunOnDestruction(LoopCallback* callback);
  void RunBeforeLoop(LoopCallback* callback);

  template<typename T> 
  bool RunInEventBaseThread(void (*fn)(T*), T* arg) {
    return RunInEventBaseThread(reinterpret_cast<void (*)(void*)>(fn),
                                reinterpret_cast<void*>(arg));
  }
  bool RunInEventBaseThread(void (*fn)(void*), void* arg);
  bool RunInEventBaseThread(const Cob& fn);

  template<typename T>
  bool RunInEventBaseThreadAndWait(void (*fn)(T*), T* arg) {
    return RunInEventBaseThreadAndWait(reinterpret_cast<void (*)(void*)>(fn),
                                       reinterpret_cast<void*>(arg));
  }
  bool RunInEventBaseThreadAndWait(void (*fn)(void*), void* arg) {
    return RunInEventBaseThreadAndWait(std::bind(fn, arg));
  }
  bool RunInEventBaseThreadAndWait(const Cob& fb);

  template<typename T>
  bool RunImmediatelyOrRunInEventBaseThreadAndWait(void(*fn)(T*), T* arg) {
    return RunImmediatelyOrRunInEventBaseThreadAndWait(reinterpret_cast<void (*)(void*)>(fn),
                                                       reinterpret_cast<void*>(arg));
  }
  bool RunImmediatelyOrRunInEventBaseThreadAndWait(void (*fn)(void*),
                                                   void *arg) {
    return RunImmediatelyOrRunInEventBaseThreadAndWait(std::bind(fn, arg));
  }
  bool RunImmediatelyOrRunInEventBaseThreadAndWait(const Cob& fn);
  
  void RunAfterDelay(const Cob& c,
                     int milliseconds,
                     TimeoutManager::InternalEnum in = TimeoutManager::InternalEnum::NORMAL);
  bool TryRunAfterDelay(const Cob& cb,
                        int milliseconds,
                        TimeoutManager::InternalEnum in = TimeoutManager::InternalEnum::NORMAL);
  
  void SetMaxLatency(int64_t max_latency, const Cob& max_latency_cob) {
    assert(enable_time_measurement_);
    max_latency_ = max_latency;
    max_latency_cob_ = max_latency_cob;
  }

  void SetLoadAvgMsec(uint32_t ms);
  void ResetLoadAvg(double value=0.0);

  double GetAvgLoopTime() const {
    assert(enable_time_measurement_);
    return avg_loop_time_.get();
  }

  bool IsRunning() const {
    return loop_thread_.load(std::memory_order_relaxed) != 0;
  }

  void WaitUntilRunning();
  int GetNotificationQueueSize() const;
  void SetMaxReadAtOnce(uint32_t max_at_once);

  bool IsInEventBaseThread() const {
    auto tid = loop_thread_.load(std::memory_order_relaxed);
    return tid == 0 || pthread_equal(tid, pthread_self());
  }

  bool InRunningEventBaseThread() const {
    return pthread_equal(loop_thread_.load(std::memory_order_relaxed),
                         pthread_self());
  }

  event_base* GetLibeventBase() const { return evb_; }
  static const char* GetLibeventVersion();
  static const char* GetLibeventMethod();

  bool BumpHandlingTime() override;

  class SmoothLoopTime {
   public:
    explicit SmoothLoopTime(uint64_t time_interval)
        : exp_coeff_(-1.0 / time_interval),
          value_(0.0),
          old_busy_leftover_(0) {
      VLOG(11) << "exp_coeff_" << exp_coeff_ << " " << __PRETTRY_FUNCTION__;
    }

    void SetTimeInterval(uint64_t time_interval);
    void Reset(double value = 0.0);
    double Get() const { return value_; }
    void Dampen(double factor)  { return value_ *= factor; }

   private:
    double exp_coeff_;
    double value_;
    int64_t old_busy_leftover_;
  };  

  void SetObserver(const std::shared_ptr<EventBaseObserver>& observer) {
    assert(enable_time_measurement_);
    observer_ = observer;
  }

  const std::shared_ptr<EventBaseObserver>& GetObserver() {
    return observer_;
  }

  void SetName(const std::string& name);
  const std::string& GetName();
  
  // Executor interface
  void Add(Cob fn) override {
    RunInEventBaseThread(fn);
  }
  // DrivableExecutor interface
  void Drive() override {
    LoopOnce();
  }

 private:
  // TiemoutManger
  void AttachTimeoutManager(AsyncTimeout* obj,
                            TimeoutManager::InternalEnum internal) override;
  void DetachTimeoutManager(AsyncTimeout* obj) override;
  bool ScheduleTimeout(AsyncTimeout* obj,
                       std::chrono::milliseconds timeout) override;
  bool CancelTimeout(AsyncTimeout* obj) override;
  bool IsInTimeoutManagerThread() override {
    return IsInEventBaseThread();
  }

  class RunInLoopCallback : public LoopCallback {
   public:
    RunInLoopCallback(void (*fn)(void*), void* arg);
    void RunLoopCallback() noexcept;

   private:
    void (*fn_)(void*);
    void* arg_;
  };

  bool NothingHandledYet();
  
  // Libevent callbacks
  static void RunFunctionPtr(std::function<void()>* fn);

  class CobTimeout : public AsyncTimeout {
   public:
    CobTimeout(EventBase* b,
               const Cob& c,
               TimeoutManager::InternalEnum in) : AsyncTimeout(b, in), cob_(c) {}

    virtual void TimeoutExpired() noexcept;
   private:
    Cob cob_;
   public:
    typedef boost::intrusive::list_member_hook<
      boost::intrusive::link_mode<boost::intrusive::auto_unlink>> ListHook;
    ListHook hook;
    typedef boost::intrusive::list<
      CobTimeout,
      boost::intrusive::member_hook<CobTimeout,
                                    ListHook,
                                    &CobTimeout::hook>,
      boost::intrusive::constant_time_size<false>> List;
  };
 
  typedef LoopCallback::List LoopCallbackList;
  class FunctionRunner;

  bool LoopBody(int flags=0);
  bool RunLoopCallbacks(bool set_context=true);
  void InitNotificationQueue();
  
  CobTimeout::List pending_cob_timeouts_;
  LoopCallbackList loop_callbacks_;
  LoopCallbackList run_before_loop_callbacks_;
  LoopCallbackList on_destruction_callbacks_;

  LoopCallbackList* run_once_callbacks_;

  bool stop_;
  std::atomic<pthread_t> loop_thread_;
  event_base* evb_;

  std::unique_ptr<NitificationQueue<std::pair<void(*)(void*)>, void*>>> queue_;
  std::unique_ptr<FunctionRunner> fn_runner_;

  int64_t max_latency_;
  
  SmoothLoopTime avg_loop_time_;
  SmoothLoopTime max_latency_loop_time_;

  Cob max_latency_cob_;

  const bool enable_time_measurement_;
  static const int kDefaultIdleWaitUsec = 20000; // 20ms
 
  uint64_t next_loop_cnt_;
  uint64_t lastest_loop_cnt_;
  uint64_t start_work_;

  std::shared_ptr<EventBaseObserver> observer_;
  uint32_t observer_sample_count_;

  std::string name_;

  DISALLOW_COPY_AND_ASSIGN(EventBase);
};

} // namespace fb
#endif // FB_EVENT_BASE_H_
