// StressTest.cc —— 多线程混合尺寸压力测试（独立 main）
// 编译：g++ -O2 -o stress_test StressTest.cc -std=c++11
// 对照：g++ -O2 -o stress_test_hash StressTest.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0

#include "ConcurrentAlloc.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

static std::atomic<int> g_bad(0);

// 生产者：只申请并写花纹；消费者：只读花纹后释放（交叉访问，逼出映射并发问题）
static void Producer(int tid, std::vector<std::pair<void*, size_t> >* out)
{
    const size_t sizes[] = {16, 64, 512, 5000, 40000, 300000, 1200000};
    for (int i = 0; i < 400; ++i)
    {
        // 尺寸下标沿用任务书给的公式。注意 i * 7 恒为 7 的倍数，所以它实际等价于
        // (tid * 3) % 7：每个 producer 固定用一个尺寸档，4 个线程覆盖 4 个档位
        // （tid=0 -> 16B，tid=1 -> 5000B，tid=2 -> 1200000B，tid=3 -> 512B）。
        // 若要真正让每个线程轮换全部 7 档，把下标改成 (i + tid * 3) % 7 即可。
        const size_t s = sizes[(i * 7 + tid * 3) % 7];
        void* p = ConcurrentAlloc(s);
        // 花纹双方统一按 (tid * 17 + i + 1) & 0xFF 归一化，避免截断差异把正确实现判错
        memset(p, (unsigned char)((tid * 17 + i + 1) & 0xFF), s);
        out->push_back(std::make_pair(p, s));
    }
}

static void Consumer(int tid, std::vector<std::pair<void*, size_t> >* in)
{
    // 消费者是全新线程，它自己那份 TLS 线程缓存还是 nullptr；而小对象释放走的是
    // ConcurrentFree -> pTLSThreadCache->Deallocate（ConcurrentAlloc.hpp 对小对象释放有
    // assert(pTLSThreadCache)，发布版则是空指针解引用）。先在本线程申请一块并立刻释放，
    // 让本线程的线程缓存惰性建立，之后才谈得上安全地"跨线程释放别人申请的块"。
    // 少了这一步，小对象（16B/512B/5000B）的释放会直接断言失败/崩溃。
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

    printf("%s  (bad=%d)\n", g_bad.load() == 0 ? "PASS" : "FAIL", g_bad.load());
    return g_bad.load() == 0 ? 0 : 1;
}
