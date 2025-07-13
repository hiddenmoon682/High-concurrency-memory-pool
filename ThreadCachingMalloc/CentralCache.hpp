#pragma once

#include "Common.hpp"
#include "PageCache.hpp"

// 每个进程中只能有一个全局的CentralCache
// 需要使用单例模式

class CentralCache
{
private:
    SpanList _spanLists[NFREELIST];
private:
    static CentralCache _sInt;      // 静态成员创建，在程序启动时创建

    // 私有化构造函数和拷贝构造
    CentralCache()
    {}
    CentralCache(const CentralCache&) = delete;
public:
    static CentralCache* GetInstance()
    {
        return &_sInt;
    }

    // 获取一个可用的Span; list是传入的链表，size是内存块的大小
    Span* GetOneSpan(SpanList& list, size_t size)
    {
        // 找现有的链表中是否有可用的Span
        Span* it = list.Begin();
        while(it != list.End())
        {
            if(it->_freeList != nullptr)
                return it;
            else
                it = it->_next;
        }

        // 因为接下来会进入到PageCache，所以在这里解锁。
        // 因此会有其他的申请内存的线程进入到这里来，它们接下来会被PageCache的锁阻塞。
        // 但是如果有其他进入该桶释放内存的线程则不会被阻塞了
        list._mtx.unlock();

        // 走到这里说明没有现成的，因此需要向PageCache里申请,需要加锁
        // 向PageCache申请时也需要确定申请的Span是包含几页的。
        PageCache::GetInstance()->_pageMtx.lock();
        Span* span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
        PageCache::GetInstance()->_pageMtx.unlock();

        // 得到新的Span后需要将大的内存块切成小块并挂到自由链表上
        // 1. 先计算这几页大块内存的起始地址
        // 2. 再计算总共大小
        // 3. 得到末尾位置
        char* start = (char*)(span->_pageId << PAGE_SHIFT); // 页号是对应虚拟地址空间对应的位置计算出来的
        size_t bytes = span->_n << PAGE_SHIFT;
        char* end = start + bytes;

        // 开始切
        // 先切一块做头，方便尾插
        span->_freeList = start;
        start += size;
        void* tail = span->_freeList;

        while(start < end)
        {
            NextObj(tail) = start;
            tail = NextObj(tail);
            start += size;
        }

        // 因为每次只有一个线程向PaegCache申请span，并且得到span之后还没有挂到Spanlist上，其他线程访问不到，因此切分时不需要加锁
        // 但是多个线程将span挂到对应的Spanlist上时需要上锁
        list._mtx.lock();

        // 将span插入list中
        list.PushFront(span);

        return span;
    }

    // 从中心缓存获取一定数量的对象给thread cache
    // start和end是多个内存块的头尾指针，batchNum是理想的需要的数量，返回值是实际返回的内存块的数量，size是内存块大小
    // CentralCache属于临界区，需要上锁
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size)
    {
        size_t index = SizeClass::Index(size);
        _spanLists[index]._mtx.lock();

        Span* span = GetOneSpan(_spanLists[index], size);
        assert(span);
        assert(span->_freeList);

        start = span->_freeList;
        end = start;
        size_t i = 0;
        size_t actualNum = 1;
        while(i < batchNum - 1 && NextObj(end) != nullptr)
        {
            ++i;
            ++actualNum;
            end = NextObj(end);
        }
        span->_freeList = NextObj(end);
        NextObj(end) = nullptr;
        span->_useCount += actualNum;

        _spanLists[index]._mtx.unlock();

        return actualNum;
    }

    // 将ThreadCache中的内存块拿回CentralCache
    // 第一个参数是自由链表，末尾指向nullptr， 第二个参数是内存块大小
    void ReleaseListToSpans(void* start, size_t size)
    {
        // 先计算是哪个桶下面的
        size_t index = SizeClass::Index(size);
        
        // 桶锁上锁
        _spanLists[index]._mtx.lock();

        while (start)
        {
            // 需要计算该内存块属于哪一个span,然后将start插入span中
            void* next = NextObj(start);

            Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
            NextObj(start) = span->_freeList;
            span->_freeList = start;
            span->_useCount--;

            // 如果span的_useCount为0，说明分配给ThreadCache的内存块已经全部还完了
            // 接下来可以尝试将span还给PageCache中并合并span了
            if(span->_useCount == 0)
            {
                // 将span从CentralCache 取下
                _spanLists[index].Erase(span);
                span->_next = nullptr;
                span->_prev = nullptr;
                span->_freeList = nullptr;

                // 由于接下来又要进PageCache，所以解锁
                _spanLists[index]._mtx.unlock();

                // 还给PageCache, 进入PageCache，上锁
                PageCache::GetInstance()->_pageMtx.lock();
                PageCache::GetInstance()->ReleaseSpanToPageCache(span);
                PageCache::GetInstance()->_pageMtx.unlock();

                _spanLists[index]._mtx.lock();
            }
            start = next;
        }

        _spanLists[index]._mtx.unlock();
    }
};

// 在类外初始化静态成员
CentralCache CentralCache::_sInt;