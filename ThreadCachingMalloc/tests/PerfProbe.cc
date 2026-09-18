// PerfProbe.cc —— 页映射替换前后的性能探针（独立 main）
//
// 这是 Task 7 用来产出 README「页映射替换前后」那张表的探针，固化进仓库以便复现。
// 用法（在 ThreadCachingMalloc 目录下；本文件用 ../ 引用库头文件，故从任何目录编译都一样）：
//   g++ -O2 -o _perf_radix tests/PerfProbe.cc -std=c++11                          # 基数树（默认 mode 1）
//   g++ -O2 -o _perf_hash  tests/PerfProbe.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0 # 对照 unordered_map
//   ./_perf_radix ; ./_perf_hash
//
// 口径：每线程先预热 10 万次申请/释放，再分别计时一轮 10 万次申请与一轮 10 万次释放；
// 输出"线程内平均延迟" = 各线程耗时之和 ÷ 总操作数，单位 ns/op。
// 申请/释放的都是 16 字节小块（走 ThreadCache -> CentralCache -> PageCache 这条主路径）。
//
// 注意：数字与本机强相关。README 的表来自 i9-12900HX，换机器绝对值会有差异
// （终审独立复现时对照侧区间低 14–24%，但方向与 ~5× 的量级一致）。

#include "../ConcurrentAlloc.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Res { double alloc_ns, free_ns; };

static Res RunPool(int nt, size_t n)
{
    std::atomic<long long> a_ns(0), f_ns(0);
    std::vector<std::thread> ts;
    for (int t = 0; t < nt; ++t)
    {
        ts.push_back(std::thread([&]() {
            std::vector<void*> v(n);
            for (size_t i = 0; i < n; ++i) v[i] = ConcurrentAlloc(16);   // 预热
            for (size_t i = 0; i < n; ++i) ConcurrentFree(v[i]);
            auto a0 = Clock::now();
            for (size_t i = 0; i < n; ++i) v[i] = ConcurrentAlloc(16);
            auto a1 = Clock::now();
            for (size_t i = 0; i < n; ++i) ConcurrentFree(v[i]);
            auto a2 = Clock::now();
            a_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(a1 - a0).count();
            f_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(a2 - a1).count();
        }));
    }
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    return Res{ (double)a_ns.load() / (nt * (double)n), (double)f_ns.load() / (nt * (double)n) };
}

int main()
{
    const size_t n = 100000;
    printf("TC_USE_RADIX_PAGEMAP=%d  每次操作的线程内平均延迟 (ns/op)\n", TC_USE_RADIX_PAGEMAP);
    printf("%6s | %-19s\n", "线程", "alloc / free");
    const int counts[] = {1, 2, 4, 8};
    for (int c = 0; c < 4; ++c)
    {
        Res p = RunPool(counts[c], n);
        printf("%6d | %8.1f / %8.1f\n", counts[c], p.alloc_ns, p.free_ns);
    }
    return 0;
}
