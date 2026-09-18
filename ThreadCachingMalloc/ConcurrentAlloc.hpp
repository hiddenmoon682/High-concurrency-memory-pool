#pragma once

#include "Common.hpp"
#include "ThreadCache.hpp"
#include "ObjectPool.hpp"

// 为了确保每个线程都有一份ThreadCache，使用TLS（线程局部存储）保证了无锁申请内存
static thread_local ThreadCache* pTLSThreadCache = nullptr;

// 申请内存
static void* ConcurrentAlloc(size_t size)
{
    // 如果要申请大于256KB的内存，则不向ThreadCahce申请
    if (size > MAX_BYTES)
    {
        // 256KB为32页，直接向PageCache申请
        size_t alignSize = SizeClass::RoundUp(size);
        size_t kpage = alignSize >> PAGE_SHIFT;

        // 进入PageCache，上锁
        PageCache::GetInstance()->_pageMtx.lock();
        Span* span = PageCache::GetInstance()->NewSpan(kpage);
        span->_objSize = alignSize;
        // 大块也必须标记为"正在使用"。否则释放时 PageCache 会把这块内存当成空闲 Span，
        // 与相邻的大块（它们的 _isUse 同样一直是 false）合并；而合并会对一个从未挂进
        // 链表的 Span 调 Erase()，其 _next/_prev 为 nullptr，直接空指针崩溃（0xC0000005），
        // 并且被误合并的活块还会被 _spanPool.Delete 回收，变成 use-after-free。
        span->_isUse = true;
        PageCache::GetInstance()->_pageMtx.unlock();

        void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
        return ptr;
    }
    else
    {
        // 每个线程都有自己的pTLSthreadcache
        if (pTLSThreadCache == nullptr)
        {
            static ObjectPool<ThreadCache> tcPool;
            // pTLSThreadCache = new ThreadCache;
            pTLSThreadCache = tcPool.New();
        }

        return pTLSThreadCache->Allocate(size);
    }
}

static void ConcurrentFree(void* ptr)
{
    Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    size_t size = span->_objSize;

    // 如果大于256KB
    if (size > MAX_BYTES)
    {
        // 释放大块同样要进 PageCache 的临界区：ReleaseSpanToPageCache 会改 _spanLists、
        // _pageMap 和 _spanPool 这些全局状态。小对象路径（CentralCache::ReleaseListToSpans）
        // 也是先拿 _pageMtx 再调用它，这里原先漏了锁，多线程下会与 NewSpan 并发读写
        // _pageMap。这里不持有桶锁，不会与上面的加锁顺序冲突。
        // 注意"免锁"只针对读侧：上面的 MapObjectToSpan（_pageMap.get）不拿任何锁，
        // 写侧的 NewSpan/ReleaseSpanToPageCache 仍然必须持 _pageMtx。
        PageCache::GetInstance()->_pageMtx.lock();
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
        PageCache::GetInstance()->_pageMtx.unlock();
    }
    else
    {
        assert(pTLSThreadCache);
        pTLSThreadCache->Deallocate(ptr, size);
    }

}