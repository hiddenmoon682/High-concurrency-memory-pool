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

#if TC_USE_RADIX_PAGEMAP

// 三层基数树：页号 -> Span*
//   Top  : INTERIOR_LENGTH 项，每项覆盖 (1<<SHIFT1) 页 = 64GB 地址空间
//   Mid  : INTERIOR_LENGTH 项，每项覆盖 (1<<SHIFT2) 页 = 16MB 地址空间
//   Leaf : LEAF_LENGTH 项，每项 1 页 = 8KB
// BITS = 35 => 2^35 页 * 8KB = 256TB > x64 用户态上限 128TB，
// 所以"页号超出映射范围"在数学上不可能发生，get() 不会因此返回 nullptr。
//
// 并发契约（重要）：
//   * get() 完全免锁。
//   * set() / clearRange() 必须由调用方持有 PageCache::_pageMtx。
//   * 不变量：某页的映射只在这块内存尚未交给调用者时才可能改变；
//     一旦交出去，在该内存活着期间映射绝不再改。因此免锁读是安全的。
//   * 写用 release、读用 acquire：x86-64 上都是普通 MOV，不产生额外指令。
class PageMap
{
public:
    static const int BITS = 35;
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
        std::lock_guard<std::mutex> lock(_mtx);
        _map[k] = v;
    }

    void clearRange(PAGE_ID start, size_t n)
    {
        std::lock_guard<std::mutex> lock(_mtx);
        for (size_t i = 0; i < n; ++i) _map.erase(start + i);
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
