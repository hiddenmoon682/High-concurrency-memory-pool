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

    printf("\n结果：%d 项通过，%d 项失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
