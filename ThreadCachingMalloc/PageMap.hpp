#pragma once

// 页号 -> Span 的映射。两种实现二选一（见 TC_USE_RADIX_PAGEMAP）：
//   1 = 三层基数树，读侧免锁（默认）
//   0 = 原来的 unordered_map + 内部锁（只用于前后对比/回退）
#ifndef TC_USE_RADIX_PAGEMAP
#define TC_USE_RADIX_PAGEMAP 1
#endif

#include "Common.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <new>
#include <unordered_map>

// 两种实现都要求 PAGE_ID 宽于 35 位（2^35 页 * 8KB = 256TB，已超出 x64 用户态上限）：
//   * 基数树实现用 (k >> BITS) 做越界守卫；
//   * 对照实现用字面量 (k >> 35) 做同样的守卫。
// 若 PAGE_ID 只有 32 位（32 位构建），这两处右移量都超过类型宽度、是**未定义行为**。
// 这里把它变成编译期错误，而不是留一处 UB。BITS 只在基数树分支里存在，所以这里用字面量。
static_assert(sizeof(PAGE_ID) * 8 > 35,
              "PageMap 要求 PAGE_ID 宽于 35 位（需要 64 位页号）");

#if TC_USE_RADIX_PAGEMAP

// 三层基数树：页号 -> Span*
//   Top  : INTERIOR_LENGTH 项，每项覆盖 (1<<SHIFT1) 页 = 64GB 地址空间
//   Mid  : INTERIOR_LENGTH 项，每项覆盖 (1<<SHIFT2) 页 = 16MB 地址空间
//   Leaf : LEAF_LENGTH 项，每项 1 页 = 8KB
// BITS = 35 => 2^35 页 * 8KB = 256TB > x64 用户态上限 128TB，
// 所以**真实运行中**"页号超出映射范围"在数学上不可能发生，get() 不会因此返回 nullptr
// （测试人为构造越界页号除外 —— PageMapTest 的 T27 就是这么做的）。
//
// 内存回收：节点（Top/Mid/Leaf）按需分配且**永不释放**。这是刻意设计——
// 免锁读要求节点在被读者拿到之后不能消失，回收就必须引入引用计数/RCU 之类的机制，
// 代价远大于收益。代价是"历史上触达过的页号区间"会一直驻留其节点。
//
// 并发契约（重要）：
//   * get() 完全免锁。
//   * set() / clearRange() 必须由调用方持有 PageCache::_pageMtx。
//   * 不变量：某页的映射只在这块内存尚未交给调用者时才可能改变；
//     一旦交出去，在该内存活着期间映射绝不再改。因此免锁读是安全的。
//     这条不变量**只覆盖"页 → Span"映射本身**：Span 的其余字段（_objSize、
//     _isUse 等）是在映射发布**之后**才写的，它们对读者可见靠的不是这里的
//     release/acquire，而是"指针交接"这件事本身建立的同步——谁拿到了那个
//     指针，谁就看到交出者在交出之前的所有写。所以不要把本契约读成
//     "整个 Span 都不可变"。
//   * 写用 release、读用 acquire：x86-64 上都是普通 MOV，不产生额外指令。
//     （例外是 clearRange 的内部读：它在 _pageMtx 临界区内、属写侧，
//     与 set() 的 release 写同处一个临界区，用 relaxed 即可。）
class PageMap
{
public:
    static const int BITS = 35;
    // 移位量必须小于 PAGE_ID 的位宽：该前提由文件顶部那条 static_assert 统一保证
    // （它对两种实现都生效，因为对照实现用的是字面量 35）。
    static const int INTERIOR_BITS = (BITS + 2) / 3;             // 12
    static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS;       // 11
    static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;       // 4096
    static const int LEAF_LENGTH = 1 << LEAF_BITS;               // 2048
    static const int SHIFT1 = INTERIOR_BITS + LEAF_BITS;         // 23
    static const int SHIFT2 = LEAF_BITS;                         // 11

    // 三级互相引用（Top -> Mid -> Leaf），需要先前置声明；
    // std::atomic<T*> 不要求 T 是完整类型，所以这样写是合法的。
    struct Mid;
    struct Leaf;

    struct Top  { std::atomic<Mid*>  kids[INTERIOR_LENGTH]; };
    struct Mid  { std::atomic<Leaf*> kids[INTERIOR_LENGTH]; };
    struct Leaf { std::atomic<Span*> values[LEAF_LENGTH]; };

    PageMap() : _nodes(0), _top(NewTop()) {}
    PageMap(const PageMap&) = delete;
    PageMap& operator=(const PageMap&) = delete;   // 隐式拷贝赋值会浅拷贝 _top/_nodes，同样必须禁止

    Span* get(PAGE_ID k) const
    {
        if ((k >> BITS) != 0) return nullptr;
        const Mid* mid = _top->kids[(k >> SHIFT1) & (INTERIOR_LENGTH - 1)].load(std::memory_order_acquire);
        if (mid == nullptr) return nullptr;
        const Leaf* leaf = mid->kids[(k >> SHIFT2) & (INTERIOR_LENGTH - 1)].load(std::memory_order_acquire);
        if (leaf == nullptr) return nullptr;
        return leaf->values[k & (LEAF_LENGTH - 1)].load(std::memory_order_acquire);
    }

    void set(PAGE_ID k, Span* v)
    {
        assert((k >> BITS) == 0);
        Leaf* leaf = EnsureLeaf(k);
        leaf->values[k & (LEAF_LENGTH - 1)].store(v, std::memory_order_release);
    }

    // 清理右开区间 [start, start+n)。只碰已存在的节点，绝不分配节点：
    // 清理一个从未触及的地址区间必须是零成本空操作。
    void clearRange(PAGE_ID start, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            const PAGE_ID k = start + i;
            if ((k >> BITS) != 0) continue;
            const Mid* mid = _top->kids[(k >> SHIFT1) & (INTERIOR_LENGTH - 1)].load(std::memory_order_relaxed);
            if (mid == nullptr) continue;
            const Leaf* leaf = mid->kids[(k >> SHIFT2) & (INTERIOR_LENGTH - 1)].load(std::memory_order_relaxed);
            if (leaf == nullptr) continue;
            const_cast<Leaf*>(leaf)->values[k & (LEAF_LENGTH - 1)].store(nullptr, std::memory_order_release);
        }
    }

    size_t nodesAllocated() const { return _nodes; }

private:
    size_t _nodes;   // 已分配的节点数（Top/Mid/Leaf 合计），仅供测试观察
    Top* _top;

    static void* NewNodeMemory(size_t bytes)
    {
        const size_t pages = (bytes + ((size_t)1 << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
        return SystemAlloc(pages);
    }

    Top* NewTop()
    {
        Top* t = new (NewNodeMemory(sizeof(Top))) Top;
        for (int i = 0; i < INTERIOR_LENGTH; ++i)
            t->kids[i].store(nullptr, std::memory_order_relaxed);
        ++_nodes;
        return t;
    }

    Mid* NewMid()
    {
        Mid* m = new (NewNodeMemory(sizeof(Mid))) Mid;
        for (int i = 0; i < INTERIOR_LENGTH; ++i)
            m->kids[i].store(nullptr, std::memory_order_relaxed);
        ++_nodes;
        return m;
    }

    Leaf* NewLeaf()
    {
        Leaf* l = new (NewNodeMemory(sizeof(Leaf))) Leaf;
        for (int i = 0; i < LEAF_LENGTH; ++i)
            l->values[i].store(nullptr, std::memory_order_relaxed);
        ++_nodes;
        return l;
    }

    Leaf* EnsureLeaf(PAGE_ID k)
    {
        const size_t i1 = (k >> SHIFT1) & (INTERIOR_LENGTH - 1);
        const size_t i2 = (k >> SHIFT2) & (INTERIOR_LENGTH - 1);

        Mid* mid = _top->kids[i1].load(std::memory_order_relaxed);
        if (mid == nullptr)
        {
            mid = NewMid();
            _top->kids[i1].store(mid, std::memory_order_release);
        }
        Leaf* leaf = mid->kids[i2].load(std::memory_order_relaxed);
        if (leaf == nullptr)
        {
            leaf = NewLeaf();
            mid->kids[i2].store(leaf, std::memory_order_release);
        }
        return leaf;
    }
};

#else   // TC_USE_RADIX_PAGEMAP == 0

// 对照实现：保留替换前的 unordered_map 行为（lookup 需要加锁）。
// 只用于前后性能对比与回退，接口与基数树版本完全一致。
//
// 加锁顺序说明：PageCache 的写路径先持 _pageMtx 再调 set()/clearRange()，
// 即顺序恒为 _pageMtx -> _mtx；get() 只拿 _mtx，且调用时不持 _pageMtx。
// 不存在 _mtx -> _pageMtx 的路径，因此不会死锁。
class PageMap
{
public:
    PageMap() {}

    Span* get(PAGE_ID k) const
    {
        std::lock_guard<std::mutex> lock(_mtx);
        Map::const_iterator it = _map.find(k);
        return it == _map.end() ? nullptr : it->second;
    }

    void set(PAGE_ID k, Span* v)
    {
        // 与基数树实现保持一致的越界防御。35 是基数树那侧 BITS 的字面量：
        // 2^35 页 * 8KB = 256TB，已在合法页号范围之外。mode 0 下没有 BITS 可用
        // （那组常量只在 #if 分支里），所以这里写字面量并说明来历。
        // 移位量 35 小于 PAGE_ID 位宽这一前提，由文件顶部那条 static_assert 保证
        // （它对两种实现都生效）；因此这里的移位不是 UB，断言是有效的。
        assert((k >> 35) == 0);
        std::lock_guard<std::mutex> lock(_mtx);
        _map[k] = v;
    }

    void clearRange(PAGE_ID start, size_t n)
    {
        std::lock_guard<std::mutex> lock(_mtx);
        // 同基数树实现：start + i 越过 2^35 页号边界时跳过。
        // 这里以**完整页号**为键，回绕出的 2^35+k 与任何范围内页号都是不同的键，
        // 所以这条跳过语句不改变可观测行为：即使去掉它，erase 也只是空操作。
        // 它保证的是语义确定：越界页不会被当成"已清理"，两种实现在越界区间上行为一致。
        // 35 的来历见 set() 的注释。
        for (size_t i = 0; i < n; ++i)
        {
            const PAGE_ID k = start + i;
            if ((k >> 35) != 0) continue;
            _map.erase(k);
        }
    }

    // 对照实现里没有"节点"概念，返回当前映射条目数
    size_t nodesAllocated() const
    {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.size();
    }

private:
    typedef std::unordered_map<PAGE_ID, Span*> Map;
    mutable std::mutex _mtx;   // 独立于 PageCache::_pageMtx，避免与写侧自锁
    Map _map;
};

#endif   // TC_USE_RADIX_PAGEMAP
