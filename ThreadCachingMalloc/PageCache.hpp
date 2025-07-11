#pragma once

#include "Common.hpp"


// PageCache 也是只能有一份，也要使用单例模式
class PageCache
{
private:
    SpanList _spanLists[NPAGES];        // 哈希桶
private:
    PageCache()
    {}
    PageCache(const PageCache&) = delete;

    static PageCache _sInst;
public:
    // 整个PageCache的锁，而不是桶锁，因为有时需要同时访问多个桶
    std::mutex _pageMtx;       

    // 获取单例对象
    static PageCache* GetInstance() 
    {
        return &_sInst;
    }

    // 获取一个K页的Span
    Span* NewSpan(size_t k)
    {
        assert(k > 0);
        assert(k < NPAGES);

        // 先去对应的桶拿Span
        if(!_spanLists[k].Empty())
        {
            return _spanLists->PopFront();
        }

        // 对应位置没有span，再检查一下后面的桶里有没有span，如果有，就把他们进行切分
        for(int i = k+1; i < NPAGES; ++i)
        {
            if(!_spanLists[i].Empty())
            {
                // 切分
                Span* nSpan = _spanLists[i].PopFront();
                Span* kSpan = new Span;

                // 在nSpan的头部切一个k页的span
                kSpan->_pageId = nSpan->_pageId;
                kSpan->_n = k;
                
                nSpan->_pageId += k;
                nSpan->_n -= k;

                // 把nSpan再挂回去
                _spanLists[nSpan->_n].PushFront(nSpan);
                // 返回kSpan
                return kSpan;
            }
        }

        // 到这里证明后面已经没有更多页的span了，需要向堆申请。
        // 向堆申请128页的大块span(128 * 8KB = 1024KB = 1MB)
        Span* bigSpan = new Span;
        void* ptr = SystemAlloc(NPAGES - 1);
        bigSpan->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
        bigSpan->_n = NPAGES - 1;

        // 挂到spanLists上去
        _spanLists[bigSpan->_n].PushFront(bigSpan);

        // 此时虽然已经有大块内存了，但是还是要返回一个K页的Span
        // 为了避免代码重复，直接递归调用
        return NewSpan(k);
    }
};

PageCache PageCache::_sInst;