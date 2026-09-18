// PageMapTest.cc —— 页号->Span 映射的单元测试（独立 main）
// 编译（基数树）：g++ -o page_map_test tests/PageMapTest.cc -std=c++11
// 编译（对照）  ：g++ -o page_map_test_hash tests/PageMapTest.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0

#include "../PageMap.hpp"

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

    // (c) n == 0：对已映射页调用 clearRange(x, 0) 必须什么都不做
    {
        PageMap m;
        Span a;
        m.set(7000, &a);
        m.clearRange(7000, 0);
        Check(m.get(7000) == &a, "T11 clearRange(x, 0) 不动已映射页");
        m.clearRange(7000, 0);
        Check(m.get(7000) == &a, "T12 clearRange(x, 0) 重复调用仍不动映射");
    }

#if TC_USE_RADIX_PAGEMAP
    // (b) clearRange 跨叶 / 跨 Mid —— 本任务交付函数的核心路径
    {
        PageMap m;
        Span a;
        Span b;
        // 跨叶：2047 与 2048 只相差 1 页，但正好落在叶边界两侧。
        // 叶下标 = (k >> 11) & 4095：2047 -> 0、2048 -> 1；两页的 mid 下标都是 0，
        // 所以它们进的是 mid[0] 下的两棵不同 Leaf —— 这才是"跨叶"要覆盖的路径。
        // （槽位下标分别是 2047 与 0。）
        m.set(2047, &a);
        m.set(2048, &b);
        const size_t before = m.nodesAllocated();   // 基线取在 set 之后：要证明的是 clearRange 不分配
        m.clearRange(2047, 2);
        Check(m.get(2047) == nullptr && m.get(2048) == nullptr, "T13 clearRange 跨叶清理两页");
        Check(m.nodesAllocated() == before, "T14 clearRange 跨叶不分配节点");
        // 跨 Mid：2^23-1 与 2^23 同样只相差 1 页，但正好落在 Mid 边界两侧。
        // mid 下标 = (k >> 23) & 4095 从 0 变成 1，所以两页进的是两棵不同的 Mid 节点；
        // 这也是这一对真正覆盖的东西：**跨 Mid 节点**。
        // (附带事实，非覆盖声明：叶下标 4095 -> 0，槽位下标 2047 -> 0。)
        const PAGE_ID midBase = (PAGE_ID)1 << SPEC_SHIFT1;
        m.set(midBase - 1, &a);
        m.set(midBase, &b);
        const size_t beforeMid = m.nodesAllocated();
        m.clearRange(midBase - 1, 2);
        Check(m.get(midBase - 1) == nullptr && m.get(midBase) == nullptr, "T15 clearRange 跨 Mid 清理两页");
        Check(m.nodesAllocated() == beforeMid, "T16 clearRange 跨 Mid 不分配节点");
    }
#endif

    {
        PageMap m;
        Span a;
        Span b;
        // 越界区间：clearRange 从 2^35-1 起、n=3，后两页越过 2^35 页号边界。
        //
        // 基数树配置下这条检查**可以失败**：页号只取低 35 位做下标，所以 2^35+k 的
        // 下标与页 k 完全相同（mid 下标 0、叶下标 0、槽位下标 k）。一旦去掉 clearRange
        // 的越界跳过，这里就会把页 0 和页 1 误清 —— 因此下面映射的正是页 0、页 1。
        //
        // 对照配置下这条检查**不可能失败**：unordered_map 以**完整页号**为键，
        // 2^35+k 与任何范围内页号都是不同的键，erase 只会空操作；那里的越界跳过
        // 只保证语义确定（越界页不会被当成"已清理"），与基数树侧保持行为一致。
        m.set(0, &a);
        m.set(1, &b);
        m.clearRange(((PAGE_ID)1 << SPEC_BITS) - 1, 3);
        Check(m.get(0) == &a && m.get(1) == &b, "T17 clearRange 越界区间被跳过、不误清低地址页");
    }

#if TC_USE_RADIX_PAGEMAP
    {
        PageMap m;
        Span a;
        const size_t before = m.nodesAllocated();
        m.clearRange(5000, 4096);            // 从未 set 过的区间
        Check(m.nodesAllocated() == before, "T18 clearRange 不分配节点");
    }
#endif

    // (a) 边界用例：每个边界页用**不同的** Span 对象。
    // 若某两个边界页混叠到同一槽位（基数树按位的切分写错时就会这样），
    // 回读到的会是"最后写入的那个 Span"，用同一个 &a 是发现不了的。
    {
        PageMap m;
        Span a;                                          // 同叶内的两个不同页共用
        Span s0, s1, s2, s3, s4, s5;                     // 六个边界页各自一个
        const PAGE_ID base = (PAGE_ID)1 << SPEC_LEAF_BITS;   // 2048：跨叶边界（16MB）
        const PAGE_ID midBase = (PAGE_ID)1 << SPEC_SHIFT1;   // 2^23：跨 Mid 边界（64GB）
        const PAGE_ID last = ((PAGE_ID)1 << SPEC_BITS) - 1;  // 2^35-1：最大合法页号

        m.set(10, &a);
        m.set(11, &a);
        // 术语约定：下文所说的"槽位下标"指 Leaf::values 的下标 = k & (LEAF_LENGTH-1)；
        // "叶下标"指 (k >> 11) & 4095，即 mid 下挂的是哪一棵 Leaf。两者不是一回事。
        m.set(0, &s0);              // 最小页号；叶下标 0、槽位下标 0
        m.set(base - 1, &s1);       // 叶边界前一页；叶下标 0、槽位下标 2047
        m.set(base, &s2);           // 叶边界后一页；叶下标 1、槽位下标 0（与 s0 同槽位、不同叶）
        m.set(midBase - 1, &s3);    // Mid 边界前一页
        m.set(midBase, &s4);        // Mid 边界后一页
        m.set(last, &s5);           // 最大合法页号

        Check(m.get(10) == &a && m.get(11) == &a, "T19 同叶多页");
        Check(m.get(0) == &s0 && m.get(base) == &s2, "T20 槽位下标相同的两个边界页各归其主");
        Check(m.get(base - 1) == &s1, "T21 跨叶边界前一页（16MB）");
        Check(m.get(base) == &s2, "T22 跨叶边界后一页（16MB）");
        Check(m.get(midBase - 1) == &s3, "T23 跨 Mid 边界前一页（64GB）");
        Check(m.get(midBase) == &s4, "T24 跨 Mid 边界后一页（64GB）");
        Check(m.get(0) == &s0, "T25 页号 0");
        Check(m.get(last) == &s5, "T26 最大页号 2^35-1");
        Check(m.get((PAGE_ID)1 << SPEC_BITS) == nullptr, "T27 越界页号返回 nullptr");
    }

    printf("\n结果：%d 项通过，%d 项失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
