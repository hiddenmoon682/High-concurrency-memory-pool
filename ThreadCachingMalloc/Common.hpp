#pragma once

#include <iostream>
#include <assert.h>
#include <algorithm>
#include <cstring>

#include <unordered_map>
#include <vector>

#include <thread>
#include <mutex>
#include <atomic>

using std::cout;
using std::endl;
using std::min;

// Common.hpp 提供内存池的公共基础组件：
// 1. SystemAlloc/SystemFree：按页向系统申请和释放内存；
// 2. FreeList：管理固定大小的空闲对象；
// 3. SizeClass：计算对象对齐后的大小和所属桶下标；
// 4. Span/SpanList：管理连续的页以及页链表。
// 整体分配路径大致是：ThreadCache -> CentralCache -> PageCache -> 系统。

#ifdef _WIN64
typedef unsigned long long PAGE_ID;
#elif _WIN32
typedef size_t PAGE_ID;
#elif __i686__
typedef size_t PAGE_ID;
#elif __LP64__
typedef unsigned long long PAGE_ID;
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__i686__) || defined(__LP64__)
#include <sys/mman.h>
#endif


static const size_t MAX_BYTES = 256 * 1024;
static const size_t NFREELIST = 200;
static const size_t NPAGES = 129;
static const size_t PAGE_SHIFT = 13; // 8 * 1024 Byte = 8 KB = 2^13 Byte

// 内存池把 8KB 作为一个管理页，页数乘以 2^13 就是申请的总字节数。
// 直接向操作系统或堆申请连续的 kpage 页。
inline static void* SystemAlloc(size_t kpage)
{
#if defined(_WIN32) || defined(_WIN64)
    void* ptr = VirtualAlloc(0, kpage << PAGE_SHIFT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(__i686__) || defined(__LP64__)
    //void* ptr = mmap(NULL, kpage << PAGE_SHIFT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* ptr = malloc(kpage << PAGE_SHIFT);
#endif
    if (ptr == nullptr)
        throw std::bad_alloc();

    return ptr;
}

// 释放 SystemAlloc 申请的整段内存。
inline static void SystemFree(void* ptr)
{
#if defined(_WIN32) || defined(_WIN64)
    VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(__i686__) || defined(__LP64__)
    // sbrk unmmap等
    free(ptr);
#endif
}


static void*& NextObj(void* obj)
{
    // 空闲对象暂时不保存用户数据，因此可以借用对象开头的几个字节，
    // 保存空闲链表中下一个对象的地址。
    // 返回引用是为了支持：NextObj(obj) = next 这样的直接修改。
    return *(void**)obj;
}

class FreeList
{
private:
    void* _freeList = nullptr;
    size_t _maxSize = 1;           // 用于慢启动调节
    size_t _size = 0;              // 自由链表挂的内存块的数量
public:
    // 头插一个空闲对象。
    void Push(void* obj)
    {
        assert(obj);
        // 头插
        NextObj(obj) = _freeList;
        _freeList = obj;
        ++_size;
    }

    // 把一段已经连接好的对象链表整体插入到头部。
    void PushRange(void* start, void* end, size_t n)
    {
        NextObj(end) = _freeList;
        _freeList = start;
        _size += n;
    }

    // 从头部取出 n 个对象，并返回这段链表的首尾指针。
    void PopRange(void*& start, void*& end, size_t n)
    {
        assert(n <= _size);
        start = _freeList;
        end = start;

        for (size_t i = 0; i < n - 1; ++i)
        {
            end = NextObj(end);
        }

        _freeList = NextObj(end);
        NextObj(end) = nullptr;
        _size -= n;
    }

    // 头删一个对象。
    void* Pop()
    {
        assert(_freeList);
        // 头删
        void* obj = _freeList;
        _freeList = NextObj(obj);
        --_size;
        return obj;
    }

    bool Empty()
    {
        return _freeList == nullptr;
    }

    size_t Size()
    {
        return _size;
    }

    size_t& MaxSize()
    {
        return _maxSize;
    }
};

// 计算对齐和计算映射
class SizeClass
{
public:
    // SizeClass 将不同的申请大小划分到有限个桶中。
    // 例如申请 13 字节时，实际按 16 字节对象管理，减少频繁计算和碎片。
    // 内碎片：第一档粒度 16 字节，最坏浪费 15 字节，相对区间上界为 16/128 = 12.5%；
    // 其余各档粒度与区间上界之比都不超过 3.2%（16/1024、128/8K、1024/64K、8K/256K）。
    //  大小                      对齐数          桶的范围 / 个数
    // [1,128]                  16byte对齐      freelist[0,8)
    // [128+1,1024]             16byte对齐      freelist[8,64)
    // [1024+1,8*1024]          128byte对齐     freelist[64,120)
    // [8*1024+1,64*1024]       1024byte对齐    freelist[120,176)
    // [64*1024+1,256*1024]     8*1024byte对齐  freelist[176,200)

    // 第一档必须是 16 字节粒度：x64 上 malloc/new 的契约要求返回 max_align_t(16) 对齐的地址。
    // 若最小粒度是 8，块大小里会出现 8 的奇数倍（如 24、40），span 基址(8KB 对齐) + k*块大小
    // 就只有一半的块能对齐到 16，SSE 的 movaps / C++17 扩展对齐 new 会直接崩。
    // 代价：最小档桶数从 16 个减到 8 个，小对象内碎片上限从 7 字节涨到 15 字节
    // （申请 1~8 字节时内存占用翻倍）。

    // 相当于 freelist[0] 是内存块为16byte的自由链表，freelist[1] 是内存块为32byte的自由链表...依次类推

    // size_t _RoundUp(size_t size, size_t alignNum)
    // {
    //     size_t alignSize;
    //     if(size % alignNum != 0)
    //     {
    //         alignSize = (size / alignNum + 1) * alignNum;
    //     }
    //     else
    //     {
    //         alignSize = size;
    //     }
    //     return alignSize;
    // }

    // 向上对齐：将 `bytes` 向上舍入到 `alignNum` 的倍数并返回结果。
    // 要求：`alignNum` 应为 2 的幂（例如 8、16、128 等），以保证位运算方式正确。
    // 实现说明：先将 bytes 加上 `(alignNum - 1)`，使其至少达到下一个对齐边界，
    // 然后用按位与 `~(alignNum - 1)` 清除低位，从而得到对齐后的值。
    // 例如：_RoundUp(13, 8) -> (13 + 7) & ~7 = 20 & ~7 = 16。
    // 注意：如果 `bytes + alignNum - 1` 导致 `size_t` 溢出，则行为未定义，
    // 调用方应避免传入极大值或先行检查边界。
    static inline size_t _RoundUp(size_t bytes, size_t alignNum)
    {
        return ((bytes + alignNum - 1) & ~(alignNum - 1));
    }

    static inline size_t RoundUp(size_t size)
    {
        // 根据不同的大小区间选择合适的对齐粒度，然后调用 `_RoundUp` 向上对齐。
        // 区间与对齐关系（尽量将内部碎片控制在较低）：
        //  [1,128]           : 16 字节对齐
        //  [129,1024]        : 16 字节对齐
        //  [1025,8*1024]     : 128 字节对齐
        //  [8*1024+1,64*1024] : 1024 字节对齐
        //  [64*1024+1,256*1024]: 8*1024 字节对齐
        //  >256*1024         : 以页大小 (1 << PAGE_SHIFT) 对齐
        // 设计意图：小对象使用较小的对齐以减少内存浪费，较大对象使用较大对齐以提高分配效率。
        if (size <= 128)
        {
            // 16 字节粒度：满足 x64 的对齐契约，同时保住足够细的桶划分
            return _RoundUp(size, 16);
        }
        else if (size <= 1024)
        {
            return _RoundUp(size, 16);
        }
        else if (size <= 8 * 1024)
        {
            return _RoundUp(size, 128);
        }
        else if (size <= 64 * 1024)
        {
            return _RoundUp(size, 1024);
        }
        else if (size <= 256 * 1024)
        {
            return _RoundUp(size, 8 * 1024);
        }
        else
        {
            // 当 size 超过 256KB 时，按页对齐（PAGE_SHIFT 定义了页大小的位移）
            return _RoundUp(size, 1 << PAGE_SHIFT);
        }
    }

    static inline size_t _Index(size_t bytes, size_t align_shift)
    {
        // 计算在一个对齐组内的零基索引（bucket index）：
        // `align_shift` 表示对齐大小的位移，即对齐大小 = (1 << align_shift)。
        // 公式说明：先将 `bytes` 加上对齐-1，完成向上取整到对齐粒度，
        // 然后右移 `align_shift` 位相当于除以对齐大小，最后减 1 得到零基索引。
        // 示例：对齐为 8（align_shift=3），bytes=8 -> ((8+7)>>3)-1 = (15>>3)-1 = 1-1 = 0（第0个桶）
        //        bytes=16 -> ((16+7)>>3)-1 = (23>>3)-1 = 2-1 = 1（第1个桶）
        // 要求与注意事项：
        // - `align_shift` 应为非负且合理（例如 3、4、7、10、13 等），对应之前约定的对齐粒度。
        // - 若 `bytes` 为 0，则结果为 ((0 + align - 1) >> shift) - 1，可能产生负值（以无符号表示为很大数），
        //   因此调用方应保证传入的 `bytes` >= 1。
        // - 当 `bytes + (1<<align_shift) - 1` 发生溢出时行为未定义，调用方应避免极端值或先行检查边界。
        return ((bytes + ((size_t)1 << align_shift) - 1) >> align_shift) - 1;
    }

    static inline size_t Index(size_t bytes)
    {
        assert(bytes <= MAX_BYTES);

        static int group_array[4] = { 8, 56, 56, 56 }; // 每个区间内桶的个数
        if (bytes <= 128)
        {
            return _Index(bytes, 4);   // 第一档 16 字节对齐，对应位移 4
        }
        else if (bytes <= 1024)
        {
            return _Index(bytes - 128, 4) + group_array[0];
        }
        else if (bytes <= 8 * 1024)
        {
            return _Index(bytes - 1024, 7) + group_array[0] + group_array[1];
        }
        else if (bytes <= 64 * 1024)
        {
            return _Index(bytes - 8 * 1024, 10) + group_array[0] + group_array[1] + group_array[2];
        }
        else if (bytes <= 256 * 1024)
        {
            return _Index(bytes - 64 * 1024, 13) + group_array[0] + group_array[1] + group_array[2] + group_array[3];
        }
        else
        {
            assert(false);
        }
        return -1;
    }

    // 用于慢启动反馈调节
    // size很大则少分配一些，size很小则多分配一些
    static size_t NumMoveSize(size_t size)
    {
        assert(size > 0);

        size_t num = MAX_BYTES / size;
        if (num <= 2)
            num = 2;

        if (num > 512)
            num = 512;

        return num;
    }

    // 计算一次向系统获取几个页
    // 单个对象 8byte
    // ...
    // 单个对象 256KB
    // size 是内存块大小，返回值是页数
    static size_t NumMovePage(size_t size)
    {
        // 计算一批内存块的数量*size得到总共的大小
        size_t num = NumMoveSize(size);
        size_t npage = num * size;
        // 除以 8KB
        npage >>= PAGE_SHIFT;
        if (npage == 0)
            npage = 1;

        return npage;
    }

};

struct Span
{
    // Span 表示一段连续的页。
    // PageCache 按页管理 Span，CentralCache 再把 Span 切分成固定大小的对象。
    // 需要注意的是，页号是直接根据系统给的实际的地址(虚拟地址)直接计算出来的，而不是从0开始的
    PAGE_ID _pageId = 0;     // 大块内存的起始页号
    size_t _n = 0;           // 页的数量

    Span* _next = nullptr;  // 双向链表的前后指针
    Span* _prev = nullptr;

    size_t _objSize = 0;    // 切好的小块内存的大小
    size_t _useCount = 0;   // 切好的小块内存，被分配给threadcache的数量

    void* _freeList = nullptr; // 当前 Span 中尚未分配出去的小对象链表

    bool _isUse = false;    // 是否仍在使用，影响 Span 能否和相邻 Span 合并
};

// 带头结点的双向链表，一个 SpanList 就是一个桶。
// PageCache 按页数分桶，CentralCache 按对象大小分桶。
class SpanList
{
private:
    Span* _head;        // 头节点
public:
    // 每个桶有自己的锁，减少不同桶之间的线程竞争。
    std::mutex _mtx;    // 桶锁
public:
    SpanList()
    {
        _head = new Span;
        _head->_next = _head;
        _head->_prev = _head;
    }

    bool Empty()
    {
        return _head->_next == _head;
    }

    Span* Begin()
    {
        return _head->_next;
    }

    Span* End()
    {
        return _head;
    }

    void PushFront(Span* newspan)
    {
        Insert(Begin(), newspan);
    }

    Span* PopFront()
    {
        Span* front = Begin();
        Erase(front);
        return front;
    }

    // 在 pos 前面插入 newspan。
    void Insert(Span* pos, Span* newspan)
    {
        assert(pos);
        assert(newspan);

        Span* prev = pos->_prev;
        newspan->_next = pos;
        newspan->_prev = prev;
        pos->_prev = newspan;
        prev->_next = newspan;
    }

    // 从链表中摘除 pos，但不释放 pos 指向的 Span。
    void Erase(Span* pos)
    {
        assert(pos);
        assert(pos != _head);

        Span* prev = pos->_prev;
        Span* next = pos->_next;

        next->_prev = prev;
        prev->_next = next;
    }

};