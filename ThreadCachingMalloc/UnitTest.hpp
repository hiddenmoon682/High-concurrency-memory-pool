#pragma once

#include "ConcurrentAlloc.hpp"


// void Alloc1()
// {   
//     std::vector<void*> v(300);
//     for(size_t i = 0; i < 300; ++i)
//     {
//         cout << i << endl;
//         v[i] = ConcurentAlloc(6);
//     }
//     for(size_t i = 0; i < 300; ++i)
//     {
//         ConcurentFree(v[i], 6);
//     }
// }

// void Alloc2()
// {   
//     std::vector<void*> v(300);
//     for(size_t i = 0; i < 300; ++i)
//     {
//         cout << i << endl;
//         v[i] = ConcurentAlloc(6);
//     }
//     for(size_t i = 0; i < 300; ++i)
//     {
//         ConcurentFree(v[i], 6);
//     }
// }

void Alloc3()
{   
    std::vector<void*> v(5);
    for(size_t i = 0; i < 5; ++i)
    {
        cout << i << endl;
        v[i] = ConcurrentAlloc(6);
    }
    for(size_t i = 0; i < 5; ++i)
    {
        ConcurrentFree(v[i]);
    }
}

void TLStest()
{
    std::thread t1(Alloc3);
    
    // std::thread t2(Alloc2);
    // t2.join();
    t1.join();
}