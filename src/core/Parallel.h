#pragma once
// Minimal fork/join helper: splits a row range over the hardware threads.

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

namespace lm {

inline int workerCount()
{
    static const int n = std::max(1u, std::thread::hardware_concurrency());
    return n;
}

// Calls fn(begin, end) on disjoint chunks of [0, count) in parallel and waits.
template <typename Fn>
void parallelRows(int count, Fn &&fn, int minChunk = 16)
{
    if (count <= 0)
        return;
    int workers = std::min(workerCount(), std::max(1, count / minChunk));
    if (workers <= 1) {
        fn(0, count);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    int chunk = (count + workers - 1) / workers;
    for (int w = 1; w < workers; ++w) {
        int b = w * chunk, e = std::min(count, b + chunk);
        if (b >= e)
            break;
        threads.emplace_back([&fn, b, e] { fn(b, e); });
    }
    fn(0, std::min(count, chunk));
    for (auto &t : threads)
        t.join();
}

} // namespace lm
