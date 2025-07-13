#pragma once

#include <iostream>
#include <assert.h>
#include <algorithm>

#include <unordered_map>

#include <thread>
#include <mutex>

using std::cout;
using std::endl;

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
static const size_t NFREELIST = 208;
static const size_t NPAGES = 129;
static const size_t PAGE_SHIFT = 13; // 8 * 1024 Byte = 8 KB = 2^13 Byte

// 直接去堆上按页申请空间
inline static void* SystemAlloc(size_t kpage)
{
#if defined(_WIN32) || defined(_WIN64)
	void* ptr = VirtualAlloc(0, kpage << PAGE_SHIFT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(__i686__) || defined(__LP64__)
    // void* ptr = mmap(NULL, kpage << PAGE_SHIFT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* ptr = malloc(kpage << PAGE_SHIFT);
#endif
	if (ptr == nullptr)
		throw std::bad_alloc();

	return ptr;
}


static void *&NextObj(void *obj)
{
    return *(void **)obj;
}

class FreeList
{
private:
    void *_freeList = nullptr;
    size_t _maxSize = 1;           // 用于慢启动调节
    size_t _size = 0;              // 自由链表挂的内存块的数量
public:
    void Push(void *obj)
    {
        assert(obj);
        // 头插
        NextObj(obj) = _freeList;
        _freeList = obj;
    }

    void PushRange(void* start, void* end, size_t n)
    {
        NextObj(end) = _freeList;
        _freeList = start;
        _size += n;
    }

    void PopRange(void*& start, void*& end, size_t n)
    {
        assert(n <= _size);
        start = _freeList;
        end = start;

        for(size_t i = 0; i < n - 1; ++i)
        {
            end = NextObj(end);
        }

        _freeList = NextObj(end);
        NextObj(end) = nullptr;
        _size -= n;
    }

    void *Pop()
    {
        assert(_freeList);
        // 头删
        void *obj = _freeList;
        _freeList = NextObj(obj);
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
    // 整体控制在最多10%左右的内碎片浪费
    //  大小                      对齐数            桶的范围 / 个数
    // [1,128]                  8byte对齐       freelist[0,16)
    // [128+1,1024]             16byte对齐      freelist[16,72)
    // [1024+1,8*1024]          128byte对齐     freelist[72,128)
    // [8*1024+1,64*1024]       1024byte对齐    freelist[128,184)
    // [64*1024+1,256*1024]      8*1024byte对齐 freelist[184,208)

    // 相当于 freelist[0] 是内存块为8byte的自由链表，freelist[1] 是内存块为16byte的自由链表...依次类推

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

    static inline size_t _RoundUp(size_t bytes, size_t alignNum)
    {
        return ((bytes + alignNum - 1) & ~(alignNum - 1));
    }

    static inline size_t RoundUp(size_t size)
    {
        if (size <= 128)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 1024)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 8 * 1024)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 64 * 1024)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 256 * 1024)
        {
            return _RoundUp(size, 8);
        }
        else
        {
            assert(false);
            return -1;
        }
    }

    // static size_t _Index(size_t bytes, size_t alignNum)
    // {
    //     if(bytes % alignNum == 0)
    //     {
    //         return bytes / alignNum - 1;
    //     }
    //     else
    //     {
    //         return bytes / alignNum;
    //     }
    // }

    static inline size_t _Index(size_t bytes, size_t align_shift)
    {
        return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
    }

    static inline size_t Index(size_t bytes)
    {
        assert(bytes <= MAX_BYTES);

        static int group_array[4] = {16, 56, 56, 56}; // 每个区间内桶的个数
        if (bytes <= 128)
        {
            return _Index(bytes, 3);
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
        if(num <= 2)
            num = 2;
        
        if(num > 512)
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
		size_t npage = num*size;
        // 除以 8KB
		npage >>= PAGE_SHIFT;
		if (npage == 0)
			npage = 1;

		return npage;
	}

};

struct Span
{
    // 需要注意的是，页号是直接根据系统给的实际的地址(虚拟地址)直接计算出来的，而不是从0开始的
    PAGE_ID _pageId = 0;     // 大块内存的起始页号
    size_t _n = 0;           // 页的数量

    Span* _next = nullptr;  // 双向链表的前后指针
    Span* _prev = nullptr;

    size_t _useCount = 0;   // 切好的小块内存，被分配给threadcache的数量
    
    void* _freeList = nullptr; // 自由链表，管理切好的小块内存
    
    bool _isUse = false;    // 判断该Span是否被使用
};

// 带头的双向链表，也就是一个“桶”
class SpanList
{
private:
    Span* _head;        // 头节点
public:
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