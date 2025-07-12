#pragma once

#include "ConcurentAlloc.hpp"


void Alloc1()
{
    for(size_t i = 0; i < 2025; ++i)
    {
        void* ptr = ConcurentAlloc(6);
        cout << i << endl;
    }
}

void Alloc2()
{
    for(size_t i = 0; i < 5; ++i)
    {
        void* ptr = ConcurentAlloc(7);
    }
}

void TLStest()
{
    std::thread t1(Alloc1);
    
    // std::thread t2(Alloc2);
    // t2.join();
    t1.join();
}