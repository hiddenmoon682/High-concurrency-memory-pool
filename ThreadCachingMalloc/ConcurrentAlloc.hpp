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
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
    }
    else
    {
        assert(pTLSThreadCache);
        pTLSThreadCache->Deallocate(ptr, size);
    }

}