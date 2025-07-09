#pragma once

#include <iostream>
#include <assert.h>
#include <thread>

using std::cout;
using std::endl;

static const int MAX_BYTES = 256 * 1024;
static const int NFREELIST = 208;

void *&NextObj(void *obj)
{
    return *(void **)obj;
}

class FreeList
{
private:
    void *_freeList = nullptr;

public:
    void Push(void *obj)
    {
        assert(obj);
        // 头插
        NextObj(obj) = _freeList;
        _freeList = obj;
    }

    void *Pop()
    {
        assert(_freeList);
        // 头删
        void *obj = _freeList;
        _freeList = NextObj(obj);
        return obj;
    }

    bool Empty()
    {
        return _freeList == nullptr;
    }
};

// 计算对齐和计算映射
class SizeClass
{
public:
    // 整体控制在最多10%左右的内碎片浪费
    //  大小                      对齐数            桶的范围 / 个数
    // [1,128]                  8byte对齐       freelist[0,16)
    // [128+1,1024]             16byte对齐      freelist[16,72)
    // [1024+1,8*1024]          128byte对齐     freelist[72,128)
    // [8*1024+1,64*1024]       1024byte对齐    freelist[128,184)
    // [64*1024+1,256*1024]      8*1024byte对齐 freelist[184,208)

    // 相当于 freelist[0] 是内存块为8byte的自由链表，freelist[1] 是内存块为16byte的自由链表...依次类推

    // size_t _RoundUp(size_t size, size_t alignNum)
    // {
    //     size_t alignSize;
    //     if(size % alignNum != 0)
    //     {
    //         alignSize = (size / alignNum + 1) * alignNum;
    //     }
    //     else
    //     {
    //         alignSize = size;
    //     }
    //     return alignSize;
    // }

    static inline size_t _RoundUp(size_t bytes, size_t alignNum)
    {
        return ((bytes + alignNum - 1) & ~(alignNum - 1));
    }

    static inline size_t RoundUp(size_t size)
    {
        if (size <= 128)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 1024)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 8 * 1024)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 64 * 1024)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 256 * 1024)
        {
            return _RoundUp(size, 8);
        }
        else
        {
            assert(false);
            return -1;
        }
    }

    // static size_t _Index(size_t bytes, size_t alignNum)
    // {
    //     if(bytes % alignNum == 0)
    //     {
    //         return bytes / alignNum - 1;
    //     }
    //     else
    //     {
    //         return bytes / alignNum;
    //     }
    // }

    static inline size_t _Index(size_t bytes, size_t align_shift)
    {
        return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
    }

    static inline size_t Index(size_t bytes)
    {
        assert(bytes <= MAX_BYTES);

        static int group_array[4] = {16, 56, 56, 56}; // 每个区间内桶的个数
        if (bytes <= 128)
        {
            return _Index(bytes, 3);
        }
        else if (bytes <= 1024)
        {
            return _Index(bytes - 128, 4) + group_array[0];
        }
        else if (bytes <= 8 * 1024)
        {
            return _Index(bytes - 1024, 7) + group_array[0] + group_array[1];
        }
        else if (bytes <= 64 * 1024)
        {
            return _Index(bytes - 8 * 1024, 10) + group_array[0] + group_array[1] + group_array[2];
        }
        else if (bytes <= 256 * 1024)
        {
            return _Index(bytes - 64 * 1024, 13) + group_array[0] + group_array[1] + group_array[2] + group_array[3];
        }
        else
        {
            assert(false);
        }
        return -1;
    }
};