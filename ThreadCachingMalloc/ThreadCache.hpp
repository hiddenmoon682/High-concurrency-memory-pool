#pragma once

#include "Common.hpp"
#include "CentralCache.hpp"

// ThreadCache 是哈希桶结构
// 需要注意的是：在计算下标选择桶的时候，不需要将size进行对齐就可以得到对应的下标
// 计算alignSize的目的是当自由链表为空，需要向CentreCache获取内存时，保证对应的内存大小符合自由链表规定的内存块大小

class ThreadCache
{
private:
    FreeList _freeLists[NFREELIST];
public:
    // 申请内存
    void* Allocate(size_t size)
    {
        assert(size < MAX_BYTES);
        // 计算申请的内存在对齐后，实际要申请的大小
        size_t alignSize = SizeClass::RoundUp(size);
        // 计算下标（位于哪个哈希桶
        size_t index = SizeClass::Index(size);
        // ThreadCache里面有就直接用，没有则向CentralCache里申请
        if(!_freeLists[index].Empty())
        {
            return _freeLists[index].Pop();
        }
        else
        {
            return FetchFromCentralCache(index, alignSize);
        }
    }
    // 释放内存
    void Deallocate(void* ptr, size_t size)
    {
        assert(ptr);
        assert(size < MAX_BYTES);

        size_t index = SizeClass::Index(size);

    }

    // 从CentralCache中申请内存
    void* FetchFromCentralCache(size_t index, size_t size)
    {
        // ThreadCache申请时需要申请一批内存块，不能太多也不能太少
        // 这里采用慢启动反馈调节算法
        // 一次一批，每次逐渐增多，直到达到上限
        int batchNum = std::min(_freeLists[index].MaxSize(), SizeClass::NumMoveSize(size));
        if(batchNum == _freeLists[index].MaxSize())
        {
            _freeLists[index].MaxSize() += 3;
        }
        // batchNum最小为4

        void* start = nullptr;
        void* end = nullptr;
        size_t actualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
        assert(actualNum > 0);

        if(actualNum == 1)
        {
            // 如果actualNum为1，则证明没有取到内存块
            assert(start == end);
            return start;
        }
        else
        {
            // 插入ThreadCache的自由链表
            _freeLists[index].PushRange(start, end);
            return start;
        }
    }
};

