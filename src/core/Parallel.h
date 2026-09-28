#pragma once
// Fork/join helper on a persistent thread pool: splits a row range over the
// hardware threads. Creating threads per call costs several milliseconds on
// Windows, so workers are started once and reused.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>
#include <thread>
#include <vector>

namespace lm {

inline int workerCount()
{
    static const int n = [] {
        // imaging kernels are memory bound: beyond ~half the logical cores (the
        // hybrid E-cores / hyper-threads) more workers only add CPU load
        int hw = int(std::max(2u, std::thread::hardware_concurrency() / 2));
        if (const char *e = std::getenv("LM_THREADS"))
            hw = std::clamp(std::atoi(e), 1, 256);
        return hw;
    }();
    return n;
}

namespace detail {

class ThreadPool {
public:
    static ThreadPool &instance()
    {
        static ThreadPool pool;
        return pool;
    }

    // Runs fn(i) for i in [0, tasks) on the pool plus the calling thread.
    void run(int tasks, const std::function<void(int)> &fn)
    {
        std::unique_lock<std::mutex> jobLock(m_jobMutex); // one job at a time
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_fn = &fn;
            m_tasks = tasks;
            m_next = 0;
            m_done = 0;
            ++m_generation;
        }
        m_cv.notify_all();
        const bool was = t_inWorker;
        t_inWorker = true; // nested parallelRows from the caller run inline
        work();            // the caller helps (never throws: exceptions are captured)
        t_inWorker = was;
        std::exception_ptr exc;
        {
            std::unique_lock<std::mutex> l(m_mutex);
            m_doneCv.wait(l, [&] { return m_done == m_tasks; });
            m_fn = nullptr;
            exc = std::exchange(m_exception, nullptr);
        }
        if (exc)
            std::rethrow_exception(exc); // after all workers finished with the caller's data
    }

    static bool inWorker() { return t_inWorker; }

private:
    ThreadPool()
    {
        const int n = workerCount() - 1;
        for (int i = 0; i < n; ++i)
            m_threads.emplace_back([this] { loop(); });
    }
    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto &t : m_threads)
            t.join();
    }

    void work()
    {
        for (;;) {
            int i;
            const std::function<void(int)> *fn;
            {
                std::lock_guard<std::mutex> l(m_mutex);
                if (!m_fn || m_next >= m_tasks)
                    return;
                i = m_next++;
                fn = m_fn;
            }
            try {
                (*fn)(i);
            } catch (...) {
                std::lock_guard<std::mutex> l(m_mutex);
                if (!m_exception)
                    m_exception = std::current_exception();
            }
            std::lock_guard<std::mutex> l(m_mutex);
            if (++m_done == m_tasks)
                m_doneCv.notify_all();
        }
    }

    void loop()
    {
        t_inWorker = true;
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(m_mutex);
                m_cv.wait(l, [&] { return m_stop || (m_generation != seen && m_fn && m_next < m_tasks); });
                if (m_stop)
                    return;
                seen = m_generation;
            }
            work();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex, m_jobMutex;
    std::condition_variable m_cv, m_doneCv;
    const std::function<void(int)> *m_fn = nullptr;
    int m_tasks = 0, m_next = 0, m_done = 0;
    uint64_t m_generation = 0;
    bool m_stop = false;
    std::exception_ptr m_exception;
    static inline thread_local bool t_inWorker = false;
};

} // namespace detail

// Calls fn(begin, end) on disjoint chunks of [0, count) in parallel and waits.
template <typename Fn>
void parallelRows(int count, Fn &&fn, int minChunk = 16)
{
    if (count <= 0)
        return;
    int chunks = std::min(workerCount() * 2, std::max(1, count / std::max(1, minChunk)));
    // nested calls from a pool worker run inline (the pool is busy with the outer job)
    if (chunks <= 1 || detail::ThreadPool::inWorker()) {
        fn(0, count);
        return;
    }
    const int chunk = (count + chunks - 1) / chunks;
    chunks = (count + chunk - 1) / chunk;
    const std::function<void(int)> task = [&](int i) {
        const int b = i * chunk, e = std::min(count, b + chunk);
        if (b < e)
            fn(b, e);
    };
    detail::ThreadPool::instance().run(chunks, task);
}

} // namespace lm
