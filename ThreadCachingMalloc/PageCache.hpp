#pragma once

#include "Common.hpp"


// PageCache 也是只能有一份，也要使用单例模式
class PageCache
{
private:
    SpanList _spanLists[NPAGES];        // 哈希桶
    std::unordered_map<PAGE_ID, Span*> _idSpanMap; // 页号到SPan的映射，用于内存回收
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

                // 存储nSpan的首尾页号跟nSpan映射，方便page cache回收内存时进行的合并查找
                _idSpanMap[nSpan->_pageId] = nSpan;
                _idSpanMap[nSpan->_pageId + nSpan->_n - 1] = nSpan;

                // 建立页号和span的映射，方便将小块内存放回Span时查找span
                for(size_t i = 0; i < k; ++i)
                {
                    _idSpanMap[kSpan->_pageId + i] = kSpan;
                }

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

    // 计算一个内存块应该属于哪个Span
    Span* MapObjectToSpan(void* obj)
    {
        // 先计算页号
        PAGE_ID id = (PAGE_ID)obj >> PAGE_SHIFT;
        // 在_idSpanMap中去找
        auto ret = _idSpanMap.find(id);
        if(ret != _idSpanMap.end())
        {
            return ret->second;
        }
        else
        {
            // 正常情况下都找得到
            assert(false);
            return nullptr;
        }
    }

    // 将Span挂回PageCache，但是由于Span有可能都被切成小块的，为了避免内存碎片
    // 将可以合并的Span合并后再挂到PageCache上
    void ReleaseSpanToPageCache(Span* span)
    {
        // 尝试向前和向后合并，解决内存碎片问题
        // 向前合并
        while (1)
        {
            // 计算PageID
            PAGE_ID id = span->_pageId - 1;
            // 在_idSpanMap中找对应的Span
            auto ret = _idSpanMap.find(id);

            // 前面的页号没找到对应的Span，不合并
            if(ret == _idSpanMap.end())
            {
                break;
            }

            Span* prevSpan = ret->second;
            // 前面的Span正在被使用，不合并
            if(prevSpan->_isUse == true)
            {
                break;
            }

            // 超出128页，无法管理，不合并
            if(prevSpan->_n + span->_n > NPAGES - 1)
            {
                break;
            }

            // 到这里说明可以合并
            span->_n += prevSpan->_n;
            span->_pageId = prevSpan->_pageId;

            // 删掉prevSpan，还有因为span都是new出来的，不要忘记delete
            _spanLists[prevSpan->_n].Erase(prevSpan);
            delete prevSpan;
        }
        
        // 向后合并
        while (1)
        {
            PAGE_ID id = span->_pageId + span->_n;
            auto ret = _idSpanMap.find(id);
            if(ret == _idSpanMap.end())
            {
                break;
            }

            Span* nextSpan = ret->second;
            if(nextSpan->_isUse == true)
            {
                break;
            }

            if(span->_n + nextSpan->_n > NPAGES - 1)
            {
                break;
            }

            // 合并
            span->_n += nextSpan->_n;

            _spanLists[nextSpan->_n].Erase(nextSpan);
            delete nextSpan;
        }

        // 将合并后的span挂上，并且为了以后方便合并，将前后PAGE_ID加进_idSpanMap
        _spanLists[span->_n].PushFront(span);
        span->_isUse = false;
        _idSpanMap[span->_pageId] = span;
        _idSpanMap[span->_pageId + span->_n - 1] = span;
    }
};

PageCache PageCache::_sInst;