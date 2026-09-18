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
        while (it != list.End())
        {
            if (it->_freeList != nullptr)
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
        span->_isUse = true;
        span->_objSize = size;
        PageCache::GetInstance()->_pageMtx.unlock();
        // 得到新的Span后需要将大的内存块切成小块并挂到自由链表上
        // 先算出这块内存的地址范围（按字节），三步：
        //  ① 起始地址：页号左移 PAGE_SHIFT(13) 位还原成真实虚拟地址（8KB 页对齐的基址）
        //  ② 总字节数：页数 × 8KB（每页 2^13 字节）
        //  ③ 结束位置：起始地址 + 总字节数（右开区间 [start, end)，不含 end 本身）
        char* start = (char*)(span->_pageId << PAGE_SHIFT); // ① 页号→地址（转 char* 才能按字节走）
        size_t bytes = span->_n << PAGE_SHIFT;              // ② 该 Span 总共几个字节
        char* end = start + bytes;                          // ③ 末尾之后的位置（循环边界）
        // 注意：页号是对应虚拟地址空间计算出来的（不是从 0 编号的），
        //       左移 13 位即可还原成系统返回的真实地址。

        // 开始切：按 size 一格一格切，块与块之间用"块内嵌 next 指针"串成自由链表
        // 先切一块做头，方便后面尾插
        span->_freeList = start;
        start += size;                    // 游标前进到第二块的起点
        void* tail = span->_freeList;     // tail 始终指向当前链表的尾块

        // 只有"能完整放下的一块"才切走：条件是 start + size <= end。
        // 若写成 start < end，只要 span 字节数除不尽 size，最后一个块的起点依然小于 end，
        // 它就会整整越出 span 尾部（最多 size-1 字节），砸到相邻 span 的第一个块上。
        // 余下不足一块的零头直接浪费（这也是为什么 span 字节数不必是 size 的整数倍）。
        while (start + size <= end)
        {
            NextObj(tail) = start;        // 尾块的 next 指向新切出的块
            tail = NextObj(tail);         // 尾巴前进
            start += size;                // 游标再前进一块
        }
        NextObj(tail) = nullptr;          // 最后一块的 next 置空，链表封尾

        // 因为每次只有一个线程向PaegCache申请span，并且得到span之后还没有挂到Spanlist上，其他线程访问不到，因此切分时不需要加锁
        // 但是多个线程将span挂到对应的Spanlist上时需要上锁
        list._mtx.lock();

        // 将span插入list中
        list.PushFront(span);

        return span;
    }

    // 从中心缓存批量获取一定数量的对象给线程缓存（批发出口）
    // start/end 是出参（引用）：摘下来的那一段链表的头、尾
    // batchNum 是"想要"的数量（由 ThreadCache 慢启动算出），size 是块大小
    // 返回值 = 实际拿到几块（可能 < batchNum，Span 里不够）
    // 注意：整个函数都在桶锁临界区内，摘取过程是原子的
    size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size)
    {
        size_t index = SizeClass::Index(size);     // ① 先算桶下标（尺寸档位）
        _spanLists[index]._mtx.lock();             // ② 拿桶锁

        // ③ 找个"有货"的 Span：优先复用桶里 _freeList 非空的；没有才向 PageCache 要新大块切好挂进来
        Span* span = GetOneSpan(_spanLists[index], size);
        assert(span);                              // 契约：必定能拿到 Span
        assert(span->_freeList);                   // 契约：它手里至少还剩一块

        // ④ 从 Span 内部的小块链表上，从头数出最多 batchNum 块
        start = span->_freeList;                   // 段头 = Span 链表头（第 1 块）
        end = start;                               // 段尾先跟段头重合
        size_t i = 0;
        size_t actualNum = 1;                      // 已包含 start 这块，所以从 1 起算
        while (i < batchNum - 1 && NextObj(end) != nullptr)
        {                                          // 两个停止条件：
            ++i;                                   //   ① 数够了"想要的-1"步（拿满 batchNum 块）
            ++actualNum;                           //   ② Span 里没货了（next 为空），货不够就少拿
            end = NextObj(end);                    //   end 沿 next 前进一块
        }

        // ⑤ 断开（只动两个指针，不遍历整条链）：
        span->_freeList = NextObj(end);            //   Span 链表头跳到段尾后面那块（余下的留在 Span 内）
        NextObj(end) = nullptr;                    //   段尾封死 → [start..end] 成为独立链表

        span->_useCount += actualNum;              // ⑥ 记账：这 actualNum 块被"借"给线程缓存了

        _spanLists[index]._mtx.unlock();           // ⑦ 解锁
        return actualNum;                          // ⑧ 实际块数（≤ batchNum）
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
            if (span->_useCount == 0)
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