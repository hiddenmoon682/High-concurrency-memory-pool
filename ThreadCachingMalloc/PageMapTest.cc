// PageMapTest.cc —— 页号->Span 映射的单元测试（独立 main）
// 编译（基数树）：g++ -o page_map_test PageMapTest.cc -std=c++11
// 编译（对照）  ：g++ -o page_map_test_hash PageMapTest.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0

#include "PageMap.hpp"

#include <cstdio>

static int g_passed = 0;
static int g_failed = 0;

// 规格固定的结构参数（BITS=35，位切分 12/12/11）。两种实现共用，用来构造
// "跨叶 / 跨中间节点"的页号，所以**不要**依赖 PageMap 的成员常量——
// 对照实现（unordered_map）没有这些常量，依赖它会让对照配置编译失败。
static const int SPEC_BITS = 35;
static const int SPEC_LEAF_BITS = 11;
static const int SPEC_SHIFT1 = 23;

static void Check(bool ok, const char* name)
{
    if (ok) { ++g_passed; printf("  PASS  %s\n", name); }
    else    { ++g_failed; printf("  FAIL  %s\n", name); }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== PageMap 单元测试 (TC_USE_RADIX_PAGEMAP=%d) ===\n", TC_USE_RADIX_PAGEMAP);

    {
        PageMap m;
        Span a, b;
        m.set(12345, &a);
        Check(m.get(12345) == &a, "T1 set/get 往返");
        m.set(12345, &b);
        Check(m.get(12345) == &b, "T2 同一页覆盖写");
        Check(m.get(12346) == nullptr, "T3 未映射的页返回 nullptr");
        Check(m.get(0) == nullptr, "T4 未映射的页号 0 返回 nullptr");
    }

#if TC_USE_RADIX_PAGEMAP
    {
        PageMap m;
        Span a;
        const size_t before = m.nodesAllocated();
        Check(m.get(999) == nullptr, "T5 未映射时 get 返回 nullptr");
        Check(m.nodesAllocated() == before, "T6 只 get 不分配节点（惰性）");
        m.set(999, &a);
        Check(m.nodesAllocated() > before, "T7 set 才分配节点");
    }
#endif

    {
        PageMap m;
        Span a;
        m.set(100, &a);
        m.set(101, &a);
        m.clearRange(100, 1);
        Check(m.get(100) == nullptr, "T8 clearRange 清掉首元素");
        Check(m.get(101) == &a, "T9 clearRange 不越界清理（右开区间）");
        m.clearRange(101, 2);
        Check(m.get(101) == nullptr, "T10 clearRange 覆盖起点");
    }

#if TC_USE_RADIX_PAGEMAP
    {
        PageMap m;
        Span a;
        const size_t before = m.nodesAllocated();
        m.clearRange(5000, 4096);            // 从未 set 过的区间
        Check(m.nodesAllocated() == before, "T11 clearRange 不分配节点");
    }
#endif

    {
        PageMap m;
        Span a;
        // 同叶内两个不同页
        m.set(10, &a);
        m.set(11, &a);
        // 跨叶边界：相差 1<<LEAF_BITS(11) = 2048 页 = 16MB
        const PAGE_ID base = (PAGE_ID)1 << SPEC_LEAF_BITS;
        m.set(base - 1, &a);
        m.set(base, &a);
        // 跨 Mid 边界：相差 1<<SHIFT1(23) = 64GB
        const PAGE_ID midBase = (PAGE_ID)1 << SPEC_SHIFT1;
        m.set(midBase - 1, &a);
        m.set(midBase, &a);
        // 边界页号
        const PAGE_ID last = ((PAGE_ID)1 << SPEC_BITS) - 1;
        m.set(0, &a);
        m.set(last, &a);

        Check(m.get(10) == &a && m.get(11) == &a, "T12 同叶多页");
        Check(m.get(base - 1) == &a && m.get(base) == &a, "T13 跨叶边界（16MB）");
        Check(m.get(midBase - 1) == &a && m.get(midBase) == &a, "T14 跨 Mid 边界（64GB）");
        Check(m.get(0) == &a, "T15 页号 0");
        Check(m.get(last) == &a, "T16 最大页号 2^35-1");
        Check(m.get((PAGE_ID)1 << SPEC_BITS) == nullptr, "T17 越界页号返回 nullptr");
    }

    printf("\n结果：%d 项通过，%d 项失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
