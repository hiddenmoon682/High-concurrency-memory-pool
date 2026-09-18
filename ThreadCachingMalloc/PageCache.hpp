#pragma once

#include "Common.hpp"
#include "ObjectPool.hpp"
#include "PageMap.hpp"

// PageCache 也是只能有一份，也要使用单例模式
class PageCache
{
private:
    // 页缓存的核心：按"页数"分桶的双向链表数组，下标就是桶内所有 Span 的页数 _n。
    // 1 页 = 2^PAGE_SHIFT = 8KB，所以下标读作"这块内存占几页"：
    //   _spanLists[0]    恒为空：不存在 0 页的 Span（NewSpan 里有 assert(k > 0)）。
    //                    留着下标 0 只是为了让"下标 == 页数"直接成立，省掉 ±1 换算。
    //   _spanLists[1]    1 页 = 8KB 的 Span，是最小单位
    //   _spanLists[k]    k 页的 Span，k ∈ [1, 128]
    //   _spanLists[128]  128 页 = 1MB，本类能管理的最大的 Span；
    //                    库存见底时 NewSpan 正是按 SystemAlloc(NPAGES - 1) 向系统要这个尺寸
    // 为什么最大只到 128 页：数组长度 NPAGES = 128 + 1，下标 129 及以上不存在。
    //   超过 128 页（> 1MB）的请求不进桶管理 —— NewSpan 直接 SystemAlloc，
    //   ReleaseSpanToPageCache 直接 SystemFree 还给系统。
    // 分配（NewSpan）怎么用这些桶：先看 _spanLists[k]；没有再往【更大】的桶扫描，
    //   找到就从它的头部切出 k 页（切剩的部分挂回 _spanLists[剩余页数]）；
    //   都没有就向系统要 128 页放进 [128]，再递归切分。
    // 释放（ReleaseSpanToPageCache）怎么用：先与相邻的空闲 Span 合并成更大的整块，
    //   再挂回 _spanLists[合并后的页数]，这样以后才能满足更大的页数请求。
    // 与 CentralCache::_spanLists[NFREELIST] 的区别：那个按"对象大小"分桶（200 个），
    //   这个按"页数"分桶（129 个）。另外这里整个类共用一把 _pageMtx，
    //   所以 SpanList 自带的 _mtx 在 PageCache 中并不使用（它是给 CentralCache 当桶锁的）。
    SpanList _spanLists[NPAGES];        // 哈希桶：下标 = 页数
    PageMap _pageMap;                   // 页号 -> Span 的映射（免锁读，见 PageMap.hpp 的并发契约；
                                        // 基数树模式免锁；对照模式内部加锁）
    ObjectPool<Span> _spanPool;
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

        // 大于32页(256KB)的直接向PageCache申请，如果它还大于128页，那就向系统堆申请
        if (k > NPAGES - 1)
        {
            void* ptr = SystemAlloc(k);
            // Span* span = new Span;
            Span* span = _spanPool.New();
            span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
            span->_n = k;

            // 方便后续释放内存
            _pageMap.set(span->_pageId, span);
            return span;
        }

        // 先去对应的桶拿Span
        if (!_spanLists[k].Empty())
        {
            // 给出Span的时候，也需要在页号映射里缓存
            Span* kSpan = _spanLists[k].PopFront();      // -----------------------

            for (PAGE_ID  i = 0; i < kSpan->_n; ++i)
            {
                _pageMap.set(kSpan->_pageId + i, kSpan);
            }

            return kSpan;
        }

        // 对应位置没有span，再检查一下后面的桶里有没有span，如果有，就把他们进行切分
        for (size_t i = k + 1; i < NPAGES; ++i)
        {
            if (!_spanLists[i].Empty())
            {
                // 切分
                Span* nSpan = _spanLists[i].PopFront();
                // Span* kSpan = new Span;
                Span* kSpan = _spanPool.New();

                // 在nSpan的头部切一个k页的span
                kSpan->_pageId = nSpan->_pageId;
                kSpan->_n = k;

                nSpan->_pageId += k;
                nSpan->_n -= k;

                // 把nSpan再挂回去
                _spanLists[nSpan->_n].PushFront(nSpan);

                // 存储nSpan的首尾页号跟nSpan映射，方便page cache回收内存时进行的合并查找
                _pageMap.set(nSpan->_pageId, nSpan);
                _pageMap.set(nSpan->_pageId + nSpan->_n - 1, nSpan);

                // 建立页号和span的映射，方便将小块内存放回Span时查找span
                for (PAGE_ID  i = 0; i < kSpan->_n; ++i)
                {
                    _pageMap.set(kSpan->_pageId + i, kSpan);
                }

                // 返回kSpan
                return kSpan;
            }
        }

        // 到这里证明后面已经没有更多页的span了，需要向堆申请。
        // 向堆申请128页的大块span(128 * 8KB = 1024KB = 1MB)
        // Span* bigSpan = new Span;
        Span* bigSpan = _spanPool.New();
        void* ptr = SystemAlloc(NPAGES - 1);
        bigSpan->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
        bigSpan->_n = NPAGES - 1;

        // 挂到spanLists上去
        _spanLists[bigSpan->_n].PushFront(bigSpan);

        // 此时虽然已经有大块内存了，但是还是要返回一个K页的Span
        // 为了避免代码重复，直接递归调用
        return NewSpan(k);
    }

    // 免锁读：free 路径不再获取 _pageMtx（这正是本次替换的目的）。
    // 安全性来自 PageMap.hpp 里写明的不变量：活跃内存对应的页映射不会再变。
    // （基数树模式免锁；对照模式内部加锁）
    Span* MapObjectToSpan(void* obj)
    {
        PAGE_ID id = (PAGE_ID)obj >> PAGE_SHIFT;
        Span* span = _pageMap.get(id);
        assert(span);
        return span;
    }

    // 将Span挂回PageCache，但是由于Span有可能都被切成小块的，为了避免内存碎片
    // 将可以合并的Span合并后再挂到PageCache上
    void ReleaseSpanToPageCache(Span* span)
    {
        // 大于128页直接释放给堆
        if (span->_n > NPAGES - 1)
        {
            _pageMap.clearRange(span->_pageId, span->_n);   // 否则首页条目成为悬垂指针
            void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
            SystemFree(ptr);

            //delete span;
            _spanPool.Delete(span);

            return;
        }

        // 尝试向前和向后合并，解决内存碎片问题
        // 向前合并
        while (1)
        {
            // 计算PageID
            PAGE_ID id = span->_pageId - 1;
            // 在 _pageMap 中找对应的 Span
            Span* prevSpan = _pageMap.get(id);

            // 前面的页号没有对应的 Span（或已被清空），不合并
            if (prevSpan == nullptr)
            {
                break;
            }

            // 前面的Span正在被使用，不合并
            if (prevSpan->_isUse == true)
            {
                break;
            }

            // 超出128页，无法管理，不合并
            if (prevSpan->_n + span->_n > NPAGES - 1)
            {
                break;
            }

            // 到这里说明可以合并
            span->_n += prevSpan->_n;
            span->_pageId = prevSpan->_pageId;

            _spanLists[prevSpan->_n].Erase(prevSpan);

            //// 删掉prevSpan，还有因为span都是new出来的，不要忘记delete
            ////delete prevSpan;
            // 现在的Span都是由定长内存池开辟的，也需要由定长内存池delete
            _spanPool.Delete(prevSpan);
        }

        // 向后合并
        while (1)
        {
            PAGE_ID id = span->_pageId + span->_n;
            Span* nextSpan = _pageMap.get(id);
            if (nextSpan == nullptr)
            {
                break;
            }

            if (nextSpan->_isUse == true)
            {
                break;
            }

            if (span->_n + nextSpan->_n > NPAGES - 1)
            {
                break;
            }

            // 合并
            span->_n += nextSpan->_n;

            _spanLists[nextSpan->_n].Erase(nextSpan);
            _spanPool.Delete(nextSpan);
        }

        // 将合并后的span挂上，并且为了以后方便合并，将前后PAGE_ID加进_pageMap
        _spanLists[span->_n].PushFront(span);
        span->_isUse = false;
        // 统一清理：包含刚被吸收的邻居区间（它们一定是新范围的子集），
        // 然后只把首/尾页指向存活下来的这个 Span。
        // 这样页缓存里的"内部页"一律是 nullptr，不会有指向已回收 Span 的悬垂条目。
        _pageMap.clearRange(span->_pageId, span->_n);
        _pageMap.set(span->_pageId, span);
        _pageMap.set(span->_pageId + span->_n - 1, span);
    }
};

PageCache PageCache::_sInst;