#pragma once

#include "Common.hpp"
#include "ThreadCache.hpp"

// 为了确保每个线程都有一份ThreadCache，使用TLS（线程局部存储）保证了无锁申请内存
static thread_local ThreadCache* pTLSThreadCache = nullptr;

// 申请内存
void* ConcurentAlloc(size_t size)
{
    // 每个线程都有自己的pTLSthreadcache
    if(pTLSThreadCache == nullptr)
    {
        pTLSThreadCache = new ThreadCache;
    }

    cout << "Thread " << std::this_thread::get_id() << ": " << pTLSThreadCache << endl;

    return pTLSThreadCache->Allocate(size);
}

void ConcurentFree(void* ptr, size_t size)
{
    assert(pTLSThreadCache);

    pTLSThreadCache->Deallocate(ptr, size);
}