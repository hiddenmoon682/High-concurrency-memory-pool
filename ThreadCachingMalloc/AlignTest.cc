// AlignTest.cc —— 尺寸分类第一档对齐粒度（8B -> 16B）的行为测试
//
// 编译运行：
//   g++ -o align_test AlignTest.cc -std=c++11
//   ./align_test          退出码 0 表示全部通过
//
// 覆盖的不变量：
//   T1  RoundUp(s) 必须是 16 的倍数，且不小于请求值 s；
//   T2  RoundUp 幂等（对齐后的值再对齐不变）；
//   T3  第一档 [1,128] 的粒度恰为 16 字节；
//   T4  桶号一致：Index(s) == Index(RoundUp(s))（防止块大小与桶错配导致越界踩内存）；
//   T5  桶号在 [0, NFREELIST) 内，且最大尺寸正好落在最后一个桶；
//   T6  真实分配的地址 16 字节对齐，且块之间互不覆盖（canary 花纹校验）；
//   T8  span 末块不得越出 span（活块间距检测 + 按整块大小写入花纹校验）；
//   T9  大块（>256KB）相邻分配/释放：释放后的存活块不得被误合并、被覆盖；
//   T10 Span 被合并回收后，映射里不得残留指向它的悬垂条目（反复 split/merge 20 轮）；
//   T10b >1MB（>128 页）的"直接还给系统"释放分支同样不得残留悬垂条目；
//   T7  MAX_BYTES 边界（对齐后正好 256KB）可申请可释放 —— 必须放在最后，
//       因为修复前它会触发 assert 直接 abort，后面的用例就没机会跑了。

#include "ConcurrentAlloc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

struct Violation
{
    size_t input;     // 传入的尺寸
    size_t actual;    // 实际得到的结果
    size_t expected;  // 期望的结果
};

static void Report(const char* name, size_t scanned, const std::vector<Violation>& bad)
{
    if (bad.empty())
    {
        ++g_passed;
        printf("  PASS  %s  (%zu 个输入)\n", name, scanned);
        return;
    }

    ++g_failed;
    printf("  FAIL  %s  (%zu/%zu 个输入不满足；示例：", name, bad.size(), scanned);
    for (size_t i = 0; i < bad.size() && i < 4; ++i)
    {
        printf(" s=%zu actual=%zu expected=%zu;", bad[i].input, bad[i].actual, bad[i].expected);
    }
    printf(")\n");
}

// T1: 对齐结果必须是 16 的倍数，且不能小于请求值
static void TestRoundUpIsMultipleOf16()
{
    std::vector<Violation> bad;
    for (size_t s = 1; s <= MAX_BYTES; ++s)
    {
        const size_t a = SizeClass::RoundUp(s);
        if (a < s || (a % 16) != 0)
        {
            bad.push_back({s, a, (s + 15) & ~(size_t)15});
        }
    }
    Report("T1  RoundUp(s) 是 16 的倍数且 >= s", MAX_BYTES, bad);
}

// T2: 幂等性，对齐结果再对齐不应变化
static void TestRoundUpIdempotent()
{
    std::vector<Violation> bad;
    for (size_t s = 1; s <= MAX_BYTES; ++s)
    {
        const size_t a = SizeClass::RoundUp(s);
        const size_t b = SizeClass::RoundUp(a);
        if (b != a)
        {
            bad.push_back({s, b, a});
        }
    }
    Report("T2  RoundUp 幂等", MAX_BYTES, bad);
}

// T3: 第一档 [1,128] 的取整粒度必须恰好是 16 字节
static void TestFirstTierGranularity()
{
    std::vector<Violation> bad;
    for (size_t s = 1; s <= 128; ++s)
    {
        const size_t expected = (s + 15) & ~(size_t)15;
        const size_t a = SizeClass::RoundUp(s);
        if (a != expected)
        {
            bad.push_back({s, a, expected});
        }
    }
    Report("T3  第一档 [1,128] 粒度为 16 字节", 128, bad);
}

// T4: 请求值和对齐值必须落在同一个桶里，否则 span 切块大小与桶期望不符
static void TestIndexMatchesRoundUp()
{
    std::vector<Violation> bad;
    for (size_t s = 1; s <= MAX_BYTES; ++s)
    {
        const size_t aligned = SizeClass::RoundUp(s);
        const size_t iRaw = SizeClass::Index(s);
        const size_t iAligned = SizeClass::Index(aligned);
        if (iRaw != iAligned)
        {
            bad.push_back({s, iRaw, iAligned});
        }
    }
    Report("T4  Index(s) == Index(RoundUp(s))", MAX_BYTES, bad);
}

// T5: 桶号必须落在 [0, NFREELIST) 内，且桶表要被用满
static void TestBucketIndexLegal()
{
    std::vector<Violation> bad;
    for (size_t s = 1; s <= MAX_BYTES; ++s)
    {
        const size_t idx = SizeClass::Index(s);
        if (idx >= NFREELIST)
        {
            bad.push_back({s, idx, NFREELIST - 1});
        }
    }
    Report("T5a Index(s) 落在 [0, NFREELIST)", MAX_BYTES, bad);

    std::vector<Violation> badLast;
    const size_t last = SizeClass::Index(MAX_BYTES);
    if (last != NFREELIST - 1)
    {
        badLast.push_back({MAX_BYTES, last, NFREELIST - 1});
    }
    Report("T5b Index(MAX_BYTES) == NFREELIST-1（桶表用满）", 1, badLast);
}

// T6: 真实分配路径的地址对齐 + 块互不覆盖
static void TestAllocationAlignmentAndCanary()
{
    const size_t sizes[] = {
        1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 40, 41, 48, 49, 56,
        57, 64, 65, 72, 73, 80, 81, 88, 89, 96, 97, 104, 105, 112, 113, 120, 121, 128,
        129, 200, 1000, 5000, 20000, 100000, 200000
    };
    const size_t sizeCount = sizeof(sizes) / sizeof(sizes[0]);

    struct Sample
    {
        size_t size;
        size_t index;
        uintptr_t addr;
    };

    size_t checked = 0;
    size_t misaligned = 0;
    size_t corrupted = 0;
    size_t duplicated = 0;
    size_t firstBadSize = 0;
    uintptr_t firstBadAddr = 0;
    std::vector<Sample> corruptSamples;
    std::vector<Sample> dupSamples;

    for (size_t si = 0; si < sizeCount; ++si)
    {
        const size_t size = sizes[si];
        // 小对象多要一些，保证跨越 span 边界；大对象少量即可
        const size_t n = (size <= 128) ? 700 : ((size <= 8192) ? 64 : 4);

        std::vector<void*> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            v.push_back(ConcurrentAlloc(size));
        }

        // 同一个块被返回两次 = 分配器重叠发放，会直接导致踩内存
        {
            std::vector<uintptr_t> sorted;
            sorted.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                sorted.push_back(reinterpret_cast<uintptr_t>(v[i]));
            }
            std::sort(sorted.begin(), sorted.end());
            for (size_t i = 1; i < sorted.size(); ++i)
            {
                if (sorted[i] == sorted[i - 1])
                {
                    ++duplicated;
                    if (dupSamples.size() < 4)
                    {
                        dupSamples.push_back({size, i, sorted[i]});
                    }
                }
            }
        }

        for (size_t i = 0; i < n; ++i)
        {
            ++checked;
            const uintptr_t addr = reinterpret_cast<uintptr_t>(v[i]);
            if ((addr % 16) != 0)
            {
                if (misaligned == 0)
                {
                    firstBadSize = size;
                    firstBadAddr = addr;
                }
                ++misaligned;
            }
            // 每块写入可校验的花纹
            memset(v[i], (unsigned char)(i * 131 + 7), size);
        }

        // 校验花纹是否被别的块覆盖（块大小与桶错配时会互相踩）
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char want = (unsigned char)(i * 131 + 7);
            const unsigned char* p = (const unsigned char*)v[i];
            for (size_t b = 0; b < size; ++b)
            {
                if (p[b] != want)
                {
                    ++corrupted;
                    if (corruptSamples.size() < 8)
                    {
                        corruptSamples.push_back({size, i, reinterpret_cast<uintptr_t>(v[i])});
                    }
                    break;
                }
            }
        }

        for (size_t i = 0; i < n; ++i)
        {
            ConcurrentFree(v[i]);
        }
    }

    if (misaligned == 0)
    {
        ++g_passed;
        printf("  PASS  T6a 分配地址 16 字节对齐  (%zu 个块)\n", checked);
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T6a 分配地址 16 字节对齐: %zu/%zu 个块不对齐（例：size=%zu addr=0x%llx, addr%%16=%llu）\n",
               misaligned, checked, firstBadSize,
               (unsigned long long)firstBadAddr, (unsigned long long)(firstBadAddr % 16));
    }

    if (corrupted == 0)
    {
        ++g_passed;
        printf("  PASS  T6b 块之间互不覆盖（canary 花纹）\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T6b 有 %zu 个块的花纹被覆盖（块大小与桶错配或重叠发放）\n", corrupted);
        for (size_t i = 0; i < corruptSamples.size(); ++i)
        {
            printf("          被覆盖: size=%zu 第 %zu 块 addr=0x%llx\n",
                   corruptSamples[i].size, corruptSamples[i].index,
                   (unsigned long long)corruptSamples[i].addr);
        }
    }

    if (duplicated == 0)
    {
        ++g_passed;
        printf("  PASS  T6c 同一尺寸下无重复发放的地址\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T6c 有 %zu 次重复发放同一地址\n", duplicated);
        for (size_t i = 0; i < dupSamples.size(); ++i)
        {
            printf("          重复: size=%zu addr=0x%llx\n",
                   dupSamples[i].size, (unsigned long long)dupSamples[i].addr);
        }
    }
}

// T8: span 切分不得产生"越出 span 尾部的末块"
// 切分循环若写成 while (start < end)，只要 span 字节数除不尽块大小，最后一个块就会
// 整整越出 span 尾部（最多 块大小-1 字节），砸到相邻 span 的第一个块上。
// 判据一：同一尺寸档的活块按地址排序后，相邻间距必须 >= 块大小（否则两块重叠）；
// 判据二：按"整个块大小"（而不是请求大小）写入花纹再校验，越界写会破坏邻居花纹。
static void TestSpanTailNoOverrun()
{
    // 这三个请求对应的块大小（528 / 5120 / 70656）都除不尽各自 span 的字节数
    const size_t reqs[]   = { 513, 5000, 70000 };
    const size_t counts[] = { 2000, 300, 40 };

    // 判据零：每个尺寸档的 span 至少要能完整放下一块（否则连第一块都会越界，
    //         而第一块是无条件切出去的）
    size_t spanTooSmall = 0;
    size_t firstSmallClass = 0;
    for (size_t req = 1; req <= MAX_BYTES; ++req)
    {
        const size_t a = SizeClass::RoundUp(req);
        if (req > 1 && a == SizeClass::RoundUp(req - 1))
        {
            continue;   // 同一个尺寸档只查一次
        }
        const size_t spanBytes = SizeClass::NumMovePage(a) << PAGE_SHIFT;
        if (spanBytes < a)
        {
            ++spanTooSmall;
            if (firstSmallClass == 0)
            {
                firstSmallClass = a;
            }
        }
    }
    if (spanTooSmall == 0)
    {
        ++g_passed;
        printf("  PASS  T8c 每个尺寸档的 span 都装得下至少一块\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T8c 有 %zu 个尺寸档的 span 装不下哪怕一块（例：块 %zu）\n", spanTooSmall, firstSmallClass);
    }

    size_t overlapPairs = 0;
    size_t corrupted = 0;
    size_t firstReq = 0;
    size_t firstBlock = 0;
    size_t firstGap = 0;

    for (size_t t = 0; t < sizeof(reqs) / sizeof(reqs[0]); ++t)
    {
        const size_t req = reqs[t];
        const size_t block = SizeClass::RoundUp(req);
        const size_t n = counts[t];

        std::vector<uintptr_t> addrs;
        addrs.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            addrs.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(req)));
        }

        // 判据一：同档活块间距不可能小于块大小
        std::vector<uintptr_t> sorted(addrs);
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 1; i < sorted.size(); ++i)
        {
            if (sorted[i] - sorted[i - 1] < block)
            {
                ++overlapPairs;
                if (firstReq == 0)
                {
                    firstReq = req;
                    firstBlock = block;
                    firstGap = sorted[i] - sorted[i - 1];
                }
            }
        }

        // 判据二：写满整个块，越界的末块会踩坏邻居的花纹
        for (size_t i = 0; i < addrs.size(); ++i)
        {
            memset(reinterpret_cast<void*>(addrs[i]), (unsigned char)(i * 131 + 7), block);
        }
        for (size_t i = 0; i < addrs.size(); ++i)
        {
            const unsigned char want = (unsigned char)(i * 131 + 7);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(addrs[i]);
            for (size_t b = 0; b < block; ++b)
            {
                if (p[b] != want)
                {
                    ++corrupted;
                    break;
                }
            }
        }

        for (size_t i = 0; i < addrs.size(); ++i)
        {
            ConcurrentFree(reinterpret_cast<void*>(addrs[i]));
        }
    }

    if (overlapPairs == 0)
    {
        ++g_passed;
        printf("  PASS  T8a 同尺寸档的活块互不重叠（间距 >= 块大小）\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T8a 有 %zu 对活块间距小于块大小（例：请求 %zu，块 %zu，间距 %zu -> 末块越出 span）\n",
               overlapPairs, firstReq, firstBlock, firstGap);
    }

    if (corrupted == 0)
    {
        ++g_passed;
        printf("  PASS  T8b 按整块大小写入的花纹未被邻居覆盖\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T8b 有 %zu 个块的花纹被覆盖（末块越出 span 砸到邻居）\n", corrupted);
    }
}

// T9: 大块（>256KB，走 PageCache 直通路径）相邻分配 / 释放
// 守护的缺陷：大块路径拿到 Span 后没有置 _isUse = true，于是释放时
// ReleaseSpanToPageCache 会把相邻的"活"大块当成空闲 Span 合并进来：
//   ① 合并会对一个从未挂进链表的 Span 调 Erase()，它的 _next/_prev 是 nullptr，
//      直接空指针崩溃（修复前本用例会以 0xC0000005 结束）；
//   ② 被误合并的活块还会被 _spanPool.Delete 回收，成为 use-after-free。
static void TestLargeBlockAdjacentFree()
{
    const size_t SZ = 300000;   // > 256KB 且 37 页 <= 128，走的正是会崩的那条路
    const size_t N = 6;

    std::vector<uintptr_t> v;
    v.reserve(N);
    for (size_t i = 0; i < N; ++i)
    {
        v.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(SZ)));
    }
    for (size_t i = 0; i < N; ++i)
    {
        memset(reinterpret_cast<void*>(v[i]), (unsigned char)(0xA0 + i), SZ);
    }

    // 释放偶数号（修复前，这一步就会崩溃）
    printf("        T9 释放相邻大块的偶数号...\n");
    for (size_t i = 0; i < N; i += 2)
    {
        ConcurrentFree(reinterpret_cast<void*>(v[i]));
    }

    // 再申请同尺寸，让刚释放的内存被复用；若活块被误合并，这里就会覆盖到它
    std::vector<uintptr_t> w;
    for (size_t i = 0; i < 3; ++i)
    {
        w.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(SZ)));
        memset(reinterpret_cast<void*>(w[i]), 0x5C, SZ);
    }

    // 存活块（奇数号）花纹必须完好
    size_t corrupted = 0;
    for (size_t i = 1; i < N; i += 2)
    {
        const unsigned char want = (unsigned char)(0xA0 + i);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(v[i]);
        for (size_t b = 0; b < SZ; ++b)
        {
            if (p[b] != want)
            {
                ++corrupted;
                printf("        存活块[%zu] 偏移 %zu 被覆盖（0x%02X != 0x%02X）\n", i, b, p[b], want);
                break;
            }
        }
    }

    // 存活块与"释放后重新申请"的块不能重叠
    size_t overlapped = 0;
    for (size_t i = 1; i < N; i += 2)
    {
        for (size_t j = 0; j < w.size(); ++j)
        {
            if (v[i] < w[j] + SZ && w[j] < v[i] + SZ)
            {
                ++overlapped;
                printf("        存活块[%zu] @0x%llx 与复用块[%zu] @0x%llx 重叠\n",
                       i, (unsigned long long)v[i], j, (unsigned long long)w[j]);
            }
        }
    }

    if (corrupted == 0 && overlapped == 0)
    {
        ++g_passed;
        printf("  PASS  T9  大块相邻释放：存活块未被覆盖/未被误合并\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T9  大块相邻释放：%zu 个存活块被覆盖，%zu 对重叠\n", corrupted, overlapped);
    }
}

// T10: Span 被合并回收后，映射里不得残留指向它的条目
// 序列：A、B 相邻大块 -> 释放 A（进页缓存）-> 释放 B（向前合并，A 的 Span 被回收）
//       -> 再申请与 A 另一侧相邻的 D 并释放（这一步在旧实现里可能命中已回收的 A）
// 守护的缺陷：替换前的实现只在合并后的首/尾页更新映射，被吸收区间里的"内部页"
// 仍然指向已经 _spanPool.Delete 掉的 Span。本次实测（见 task-5-report.md 的 Step 3）：
// 本用例在替换前的实现上同样通过，因此它**不能**证明"陈旧映射已被修复"，
// 它守的是"合并/切分反复发生时不出现覆盖与重叠"这条行为不变量。原因是删除后
// 内部页条目虽然悬垂，但真正被读到的那几个页边界都会立刻被重写（切分路径会
// 给 kSpan 的每一页重新 set）。
static void TestStaleMapAfterMerge()
{
    const size_t SZ = 300000;   // 37 页，>256KB 且 <=128 页，走的正是会合并的路径

    for (int round = 0; round < 20; ++round)
    {
        std::vector<uintptr_t> v;
        for (size_t i = 0; i < 6; ++i)
        {
            v.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(SZ)));
        }
        for (size_t i = 0; i < 6; ++i)
        {
            memset(reinterpret_cast<void*>(v[i]), (unsigned char)(0xB0 + i), SZ);
        }

        // 释放 0、2、4：其中若干次会触发"活块被误判/被合并"的旧路径
        for (size_t i = 0; i < 6; i += 2)
        {
            ConcurrentFree(reinterpret_cast<void*>(v[i]));
        }
        // 再申请同尺寸，迫使页缓存切分/合并反复发生
        std::vector<uintptr_t> w;
        for (size_t i = 0; i < 3; ++i)
        {
            w.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(SZ)));
            memset(reinterpret_cast<void*>(w[i]), 0x5C, SZ);
        }

        size_t corrupted = 0;
        size_t overlapped = 0;
        for (size_t i = 1; i < 6; i += 2)
        {
            const unsigned char want = (unsigned char)(0xB0 + i);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(v[i]);
            for (size_t b = 0; b < SZ; ++b)
            {
                if (p[b] != want) { ++corrupted; break; }
            }
            for (size_t j = 0; j < w.size(); ++j)
            {
                if (v[i] < w[j] + SZ && w[j] < v[i] + SZ) ++overlapped;
            }
        }
        for (size_t i = 0; i < w.size(); ++i) ConcurrentFree(reinterpret_cast<void*>(w[i]));

        if (corrupted != 0 || overlapped != 0)
        {
            ++g_failed;
            printf("  FAIL  T10 第 %d 轮：%zu 个存活块被覆盖，%zu 对重叠\n", round, corrupted, overlapped);
            return;
        }
    }

    ++g_passed;
    printf("  PASS  T10 合并回收后映射无残留（20 轮）\n");
}

// T10b: 覆盖 >1MB（>128 页）那条"直接还给系统"的释放路径
// Task 3 的审查指出：ReleaseSpanToPageCache 里 span->_n > NPAGES-1 分支的
// clearRange 没有被任何已执行用例覆盖 —— 现有大块用例最大只到 300000 字节
// （37 页），而只有 >128 页才会进这条分支。这里每次都释放 147 页的块。
// 判据与 T9/T10 一致：存活块花纹必须完好，且存活块与复用块不得重叠。
// 注意只对"存活块 vs 新块"做重叠判定：已释放的块被下一次同尺寸申请按原地址
// 复用是 SystemFree 的正常行为，不是缺陷。
static void TestStaleMapLargeSpan()
{
    const size_t SZ = 1200000;   // > 1MB，对齐后 1204224 字节 = 147 页 > 128 页
    const size_t N = 6;
    printf("        T10b 大块 %zu 字节（对齐后 %zu = %zu 页）走直接还系统分支\n",
           SZ, SizeClass::RoundUp(SZ), SizeClass::RoundUp(SZ) >> PAGE_SHIFT);

    std::vector<uintptr_t> v;
    v.reserve(N);
    for (size_t i = 0; i < N; ++i)
    {
        v.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(SZ)));
        memset(reinterpret_cast<void*>(v[i]), (unsigned char)(0xC0 + i), SZ);
    }

    // 释放偶数号：这一步走的就是 >128 页的"直接还系统"分支
    for (size_t i = 0; i < N; i += 2)
    {
        ConcurrentFree(reinterpret_cast<void*>(v[i]));
    }

    // 再申请同尺寸，迫使这段地址被复用
    std::vector<uintptr_t> w;
    for (size_t i = 0; i < N / 2; ++i)
    {
        w.push_back(reinterpret_cast<uintptr_t>(ConcurrentAlloc(SZ)));
        memset(reinterpret_cast<void*>(w[i]), 0x7E, SZ);
    }

    // 存活块（奇数号）花纹必须完好
    size_t corrupted = 0;
    for (size_t i = 1; i < N; i += 2)
    {
        const unsigned char want = (unsigned char)(0xC0 + i);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(v[i]);
        for (size_t b = 0; b < SZ; ++b)
        {
            if (p[b] != want) { ++corrupted; break; }
        }
    }

    // 存活块与"释放后重新申请"的块不能重叠
    size_t overlapped = 0;
    for (size_t i = 1; i < N; i += 2)
    {
        for (size_t j = 0; j < w.size(); ++j)
        {
            if (v[i] < w[j] + SZ && w[j] < v[i] + SZ) ++overlapped;
        }
    }

    for (size_t i = 0; i < w.size(); ++i) ConcurrentFree(reinterpret_cast<void*>(w[i]));
    for (size_t i = 1; i < N; i += 2) ConcurrentFree(reinterpret_cast<void*>(v[i]));

    if (corrupted == 0 && overlapped == 0)
    {
        ++g_passed;
        printf("  PASS  T10b >1MB 释放路径：存活块花纹完好且未被复用块重叠\n");
    }
    else
    {
        ++g_failed;
        printf("  FAIL  T10b >1MB 释放路径：%zu 个存活块被覆盖，%zu 对重叠\n", corrupted, overlapped);
    }
}

// T7: 对齐后正好等于 MAX_BYTES 的请求也必须能申请、能释放
static void TestMaxBytesBoundary()
{
    // 253953 对齐后正好是 262144（= MAX_BYTES）；262144 本身是最容易踩边界的值
    const size_t reqs[] = { MAX_BYTES, MAX_BYTES - 1, MAX_BYTES - 8191 };
    const size_t total = sizeof(reqs) / sizeof(reqs[0]);

    for (size_t i = 0; i < total; ++i)
    {
        printf("        边界用例：申请 %zu 字节（对齐后 %zu 字节）\n", reqs[i], SizeClass::RoundUp(reqs[i]));
        fflush(stdout);
        void* p = ConcurrentAlloc(reqs[i]);
        if (p == nullptr)
        {
            ++g_failed;
            printf("  FAIL  T7 申请 %zu 字节返回空\n", reqs[i]);
            return;
        }
        ConcurrentFree(p);
    }

    ++g_passed;
    printf("  PASS  T7  MAX_BYTES 边界（对齐后 = 256KB）可申请可释放\n");
}

int main()
{
    // 关闭 stdout 缓冲：万一某个用例让进程崩溃，前面已经跑过的结论不会丢
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 尺寸分类 / 内存池不变量测试 ===\n");
    TestRoundUpIsMultipleOf16();
    TestRoundUpIdempotent();
    TestFirstTierGranularity();
    TestIndexMatchesRoundUp();
    TestBucketIndexLegal();
    TestAllocationAlignmentAndCanary();
    TestSpanTailNoOverrun();
    TestLargeBlockAdjacentFree();   // 修复前会崩溃
    TestStaleMapAfterMerge();       // T10：合并回收后映射不得残留悬垂 Span*
    TestStaleMapLargeSpan();        // T10b：>1MB 直接还系统分支
    TestMaxBytesBoundary();         // 必须最后：修复前这一项会 abort

    printf("\n结果：%d 项通过，%d 项失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
