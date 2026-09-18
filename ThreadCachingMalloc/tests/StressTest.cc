// StressTest.cc —— 多线程混合尺寸压力测试（独立 main）
// 编译：g++ -O2 -o stress_test tests/StressTest.cc -std=c++11
// 对照：g++ -O2 -o stress_test_hash tests/StressTest.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0

#include "../ConcurrentAlloc.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

static const int kNClasses = 7;
static const size_t kSizes[kNClasses] = {16, 64, 512, 5000, 40000, 300000, 1200000};

static std::atomic<int> g_bad(0);
// 每个尺寸档实际分配了多少块：这是"混合尺寸"的覆盖度证据，
// 也是防止取尺寸公式将来退化（例如又变成某个线程永远只用一个档）的检查点。
static std::atomic<int> g_classCount[kNClasses];

// 生产者：只申请并写花纹；消费者：只读花纹后释放（交叉访问，逼出映射并发问题）
static void Producer(int tid, std::vector<std::pair<void*, size_t> >* out)
{
    for (int i = 0; i < 400; ++i)
    {
        // 每个 producer 轮换全部 7 个尺寸档：i 每步 +1，所以 (i + tid * 3) % 7 必然遍历 0..6；
        // 加 tid * 3 只是错开各线程的起始档，避免 4 个线程总是同时抢同一个档位。
        const int c = (i + tid * 3) % kNClasses;
        const size_t s = kSizes[c];
        void* p = ConcurrentAlloc(s);
        // 花纹双方统一按 (tid * 17 + i + 1) & 0xFF 归一化，避免截断差异把正确实现判错
        memset(p, (unsigned char)((tid * 17 + i + 1) & 0xFF), s);
        out->push_back(std::make_pair(p, s));
        g_classCount[c].fetch_add(1, std::memory_order_relaxed);
    }
}

static void Consumer(int tid, std::vector<std::pair<void*, size_t> >* in)
{
    // 这一行不是冗余，删掉测试又会崩。库的既有缺陷（与本次基数树/页映射替换无关，
    // 已进 backlog）："从未分配过内存的线程无法释放小对象"——消费者跑在全新线程上，
    // 它那份 thread_local pTLSThreadCache 还是 nullptr，而 ConcurrentFree 释放小对象时
    // 直接调用 pTLSThreadCache->Deallocate(...)：debug 下 ConcurrentAlloc.hpp 里那句
    // assert(pTLSThreadCache) 会失败并 abort，release 下则是空指针解引用。
    // 这里只做预热规避：先在本线程申请一块并立刻释放，让本线程的线程缓存惰性建立，
    // 之后才能安全地跨线程释放别人申请的块。
    ConcurrentFree(ConcurrentAlloc(16));

    for (size_t k = 0; k < in->size(); ++k)
    {
        // 与 Producer 的写入公式逐字节一致（同为 & 0xFF 归一化后的值）
        const unsigned char want = (unsigned char)((tid * 17 + (int)k + 1) & 0xFF);
        const unsigned char* q = (const unsigned char*)(*in)[k].first;
        for (size_t b = 0; b < (*in)[k].second; ++b)
        {
            if (q[b] != want) { ++g_bad; break; }
        }
        ConcurrentFree((*in)[k].first);
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== 多线程压力测试 (TC_USE_RADIX_PAGEMAP=%d) ===\n", TC_USE_RADIX_PAGEMAP);

    const int kThreads = 4;
    std::vector<std::vector<std::pair<void*, size_t> > > store(kThreads);
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) producers.push_back(std::thread(Producer, t, &store[t]));
    for (size_t i = 0; i < producers.size(); ++i) producers[i].join();

    std::vector<std::thread> consumers;
    for (int t = 0; t < kThreads; ++t) consumers.push_back(std::thread(Consumer, t, &store[t]));
    for (size_t i = 0; i < consumers.size(); ++i) consumers[i].join();

    // 覆盖度证据：打印每个尺寸档实际分配到的块数。任何一档为 0 都判失败——那样就不再是
    // "混合尺寸"压力测试，这一步也让取尺寸公式的退化重新变得可见（原公式就退化成过每线程一档）。
    int uncovered = 0;
    printf("尺寸档覆盖:");
    for (int c = 0; c < kNClasses; ++c)
    {
        const int n = g_classCount[c].load();
        if (n == 0) ++uncovered;
        printf(" %zuB=%d", kSizes[c], n);
    }
    printf("\n");
    if (uncovered != 0)
    {
        printf("FAIL: 有 %d 个尺寸档从未被分配（混合尺寸覆盖不完整）\n", uncovered);
    }

    const int bad = g_bad.load();
    printf("%s  (bad=%d)\n", (bad == 0 && uncovered == 0) ? "PASS" : "FAIL", bad);
    return (bad == 0 && uncovered == 0) ? 0 : 1;
}
