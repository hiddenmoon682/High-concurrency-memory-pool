#include <iostream>

// 定长内存池

template <class T>
class ObjectPool
{
private:
    char *_memory = nullptr;   // 大块的内存空间
    void *_freelist = nullptr; // 链表，管理还回来的内存块
    int _remainBytes = 0;      // 剩下的空间
public:
    T *New()
    {
        T *obj = nullptr;
        // 如果链表中有还回来的内存块就直接分配出去
        if (_freelist != nullptr)
        {
            obj = (T*)_freelist;
            _freelist = *(void **)_freelist;
        }
        else
        {
            if (_remainBytes < sizeof(T))
            {
                _remainBytes = 128 * 1024;
                _memory = (char *)malloc(_remainBytes); // 开辟128KB
                if (_memory == nullptr)
                {
                    throw std::bad_alloc();
                }
            }

            obj = (T *)_memory;
            // 如果对象大小小于一个指针的大小，就直接分配一个指针大小的块
            size_t objsize = sizeof(T) < sizeof(void *) ? sizeof(void *) : sizeof(T);
            _memory += objsize;
            _remainBytes -= objsize;
        }

        // 定位new，在已经开辟的内存上构造对象
        new (obj)T;

        return obj;
    }

    void Delete(T *obj)
    {  
        // 显示调用T的析构函数
        obj->~T();

        *(void**)obj = _freelist;
        _freelist = obj; 
    }
};
