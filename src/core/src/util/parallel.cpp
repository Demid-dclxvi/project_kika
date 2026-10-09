#include "util/parallel.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kika {

namespace {

thread_local bool tl_inside = false;  // поток уже выполняет задачу пула

int default_threads() {
  if (const char* env = std::getenv("KIKA_THREADS")) {
    try {
      const int n = std::stoi(env);
      if (n > 0) return std::min(n, 256);
    } catch (...) {
    }
  }
  const unsigned hw = std::thread::hardware_concurrency();
  return static_cast<int>(std::clamp(hw == 0 ? 1u : hw, 1u, 32u));
}

class Pool {
 public:
  int size() const { return wanted_.load(); }

  void resize(int n) {
    std::lock_guard run_lock(run_m_);
    wanted_ = n <= 0 ? default_threads() : n;
    stop_workers();
  }

  void run(std::size_t n_tasks, const std::function<void(std::size_t)>& fn) {
    if (n_tasks == 0) return;
    if (tl_inside || wanted_.load() <= 1 || n_tasks == 1) {
      for (std::size_t k = 0; k < n_tasks; ++k) fn(k);
      return;
    }
    std::lock_guard run_lock(run_m_);
    ensure_workers();
    {
      std::unique_lock lk(m_);
      cv_done_.wait(lk, [&] { return active_ == 0; });
      job_ = &fn;
      n_tasks_ = n_tasks;
      next_.store(0);
      done_ = 0;
      error_ = nullptr;
      ++generation_;
    }
    cv_work_.notify_all();
    tl_inside = true;
    process(fn, n_tasks);
    tl_inside = false;
    std::exception_ptr err;
    {
      std::unique_lock lk(m_);
      cv_done_.wait(lk, [&] { return done_ == n_tasks && active_ == 0; });
      err = error_;
      job_ = nullptr;
    }
    if (err) std::rethrow_exception(err);
  }

 private:
  void process(const std::function<void(std::size_t)>& fn, std::size_t n) {
    std::size_t local = 0;
    for (;;) {
      const std::size_t k = next_.fetch_add(1);
      if (k >= n) break;
      try {
        fn(k);
      } catch (...) {
        std::lock_guard lk(m_);
        if (!error_) error_ = std::current_exception();
      }
      ++local;
    }
    if (local) {
      std::lock_guard lk(m_);
      done_ += local;
    }
  }

  void worker() {
    tl_inside = true;
    std::uint64_t seen = 0;  // новый поток подхватит уже идущую задачу, если она есть
    for (;;) {
      const std::function<void(std::size_t)>* job = nullptr;
      std::size_t n = 0;
      {
        std::unique_lock lk(m_);
        cv_work_.wait(lk, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
        if (!job_) continue;  // задача уже завершена — опоздали
        job = job_;
        n = n_tasks_;
        ++active_;
      }
      process(*job, n);
      {
        std::lock_guard lk(m_);
        --active_;
      }
      cv_done_.notify_all();
    }
  }

  void ensure_workers() {
    const int need = wanted_.load() - 1;
    if (static_cast<int>(threads_.size()) == need) return;
    stop_workers();
    for (int i = 0; i < need; ++i) threads_.emplace_back([this] { worker(); });
  }

  void stop_workers() {
    {
      std::lock_guard lk(m_);
      stop_ = true;
    }
    cv_work_.notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();
    std::lock_guard lk(m_);
    stop_ = false;
  }

  std::atomic<int> wanted_{default_threads()};
  std::mutex run_m_;
  std::mutex m_;
  std::condition_variable cv_work_, cv_done_;
  std::vector<std::thread> threads_;
  const std::function<void(std::size_t)>* job_ = nullptr;
  std::size_t n_tasks_ = 0;
  std::atomic<std::size_t> next_{0};
  std::size_t done_ = 0;
  int active_ = 0;
  std::uint64_t generation_ = 0;
  std::exception_ptr error_;
  bool stop_ = false;
};

// Пул намеренно не разрушается: потоки завершатся вместе с процессом
// (так нет риска зависнуть на join в статических деструкторах).
Pool& pool() {
  static Pool* p = new Pool();
  return *p;
}

}  // namespace

int thread_count() { return pool().size(); }

void set_thread_count(int n) { pool().resize(n); }

namespace util {

void run_tasks(std::size_t n_tasks, const std::function<void(std::size_t)>& fn) { pool().run(n_tasks, fn); }

}  // namespace util

}  // namespace kika
