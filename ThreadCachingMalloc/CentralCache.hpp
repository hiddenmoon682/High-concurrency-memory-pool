#pragma once

#include "Common.hpp"

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
        return nullptr;
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

        _spanLists[index]._mtx.unlock();

        return actualNum;
    }
};

// 在类外初始化静态成员
CentralCache CentralCache::_sInt;