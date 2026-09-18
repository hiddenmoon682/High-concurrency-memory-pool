# High-concurrency-memory-pool

高并发内存池 —— 参考 Google tcmalloc 设计思路，用 C++11 从零实现的多线程内存分配器。
核心目标是降低多线程下 `malloc`/`free` 的**锁竞争**：通过**线程局部缓存（TLS）**让大多数申请/释放走无锁路径，
只有缓存不足或过剩时才与共享的**中心缓存 / 页缓存**交互。

仓库包含两个可独立构建的子项目：

| 目录 | 内容 | 定位 |
|------|------|------|
| `ThreadCachingMalloc/` | 高并发内存池（主体） | 三层缓存架构 + 单元测试 + 性能对比基准 |
| `Fixed_length_memory_pool/` | 定长对象池（模板） | 前置练习：大块内存 + 空闲链表复用 |

---

## 一、设计：三层缓存架构（ThreadCachingMalloc）

```
线程申请
   │
   ▼
┌─────────────┐   无锁（TLS），大多数分配/释放在这层完成
│ ThreadCache │   每线程一份哈希桶：200 个自由链表（FreeList）
└──────┬──────┘
       │ 批量获取 / 批量归还（慢启动调节）
       ▼
┌─────────────┐   桶锁（每桶一把），多线程共享
│ CentralCache│   Span 切成小块内存，跨线程流动
└──────┬──────┘
       │ 按页申请 / 回收合并
       ▼
┌─────────────┐   全局互斥锁，单例
│  PageCache  │   页级 Span 管理（129 个桶），页号→Span 映射
└──────┬──────┘
       │ SystemAlloc（Windows: VirtualAlloc / Linux: malloc）
       ▼
      系统堆
```

### 各模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| 公共定义 | `Common.hpp` | 常量（`MAX_BYTES=256KB`、`NFREELIST=200`、页大小 8KB）、`FreeList`、`Span`/`SpanList`、`SizeClass` 对齐与桶号映射、`SystemAlloc`/`SystemFree` |
| 线程缓存 | `ThreadCache.hpp` | 每线程 TLS 哈希桶；桶空时向中心缓存**批量**申请（慢启动反馈调节，逐步增大批次）；桶过长时批量归还 |
| 中心缓存 | `CentralCache.hpp` | 桶锁粒度并发；管理切好的小块内存（Span 内自由链表），`FetchRangeObj`/`ReleaseListToSpans` |
| 页缓存 | `PageCache.hpp` | 单例；页级 Span 双向链表桶 + **页号→Span 映射**（回收时反查），大 span 按需切分，相邻页合并归还 |
| 基数树 | `PageMap.hpp` | 页号→Span 映射：三层基数树（Top/Mid/Leaf，节点惰性分配），读侧免锁（radix tree） |
| 定长对象池 | `ObjectPool.hpp` | 大块内存 + 空闲链表复用，定位 `new` 构造；用于分配内部对象（Span、ThreadCache） |
| 公开接口 | `ConcurrentAlloc.hpp` | `ConcurrentAlloc(size)` / `ConcurrentFree(ptr)`，TLS 惰性初始化 |
| 单元测试 | `UnitTest.hpp` | `TLStest` 等多线程申请/释放正确性用例（`main.cc` 入口） |
| 基准测试 | `BenchMark.cc` | 多线程并发下与系统 `malloc` 的耗时对比 |

### 关键设计点

- **分段对齐**：`[1,128]` 按 16B、`[128,1K]` 16B、`[1K,8K]` 128B、`[8K,64K]` 1024B、`[64K,256K]` 8KB 对齐；
  第一档取 16B 是为了满足 x64 上 `malloc`/`new` 的 `max_align_t`(16 字节) 对齐契约——若用 8B 粒度，块大小会出现 8 的奇数倍（24、40、56…），
  「span 基址(8KB 对齐) + k×块大小」就只有一半的块能对齐到 16，SSE 的 `movaps` 与 C++17 扩展对齐 `new` 会直接崩；
  代价是最小桶由 16 个减到 8 个、小对象内碎片上限由 7 字节涨到 15 字节（相对区间上界 16/128 = 12.5%，申请 1~8 字节时占用翻倍）；
- **慢启动批量调节**：线程缓存每次向中心缓存申请一批（`NumMoveSize` 计算），越常用批次越大（`MaxSize` 递增），减少跨层交互；
- **大块直通**：申请 > 256KB 时跳过线程/中心缓存，直接按页向 PageCache/系统申请（正好 256KB 仍走尺寸档，是最后一个桶）；
- **回收与合并**：释放时经页号→Span 映射反查归属，PageCache 将相邻空闲页合并成大 span（内存回收模块）；
- **平台兼容**：Windows 走 `VirtualAlloc`/`VirtualFree`，Linux/macOS 走系统分配。
- **页映射可切换**：`PageMap.hpp` 顶部 `TC_USE_RADIX_PAGEMAP`（默认 1）选用三层基数树（读侧免锁）；
  用 `-DTC_USE_RADIX_PAGEMAP=0` 可切回原来的 `unordered_map` 实现做前后对比。
  两点注意：① 关闭开关（mode 0）时**读会串行化**——对照实现内部有互斥锁，每次 `get` 都要加锁，
  这正是下面 A/B 对比要观察的那段差异；② **同一个程序的所有 TU 必须用同一个开关值**：
  `PageMap.hpp` 有 `#pragma once`，先被包含的那个版本会决定整个程序里的 `PageMap` 定义，
  两种模式的定义不兼容，混用是**静默的 ODR 违规**（不报错、行为未定义），所以 `-D` 只应加在整个构建上，
  不要只加到某一个源文件；

---

## 二、定长内存池（Fixed_length_memory_pool）

`ObjectPool<T>` 模板：首次按 128KB 整块 `malloc`，之后从大块内存切分（小于指针大小的对象按指针大小分配）；
释放的对象挂入空闲链表复用，避免频繁向系统申请；`New()` 内用**定位 new** 构造对象，`Delete()` 显式析构后回收。
`main.cc` 内置与 `new`/`delete` 的性能对比用例（`TreeNode` × 10 万 × 3 轮）。

---

## 三、构建与运行

环境：g++（C++11），无需第三方依赖。

```bash
# 高并发内存池 —— 性能基准（对比系统 malloc）
cd ThreadCachingMalloc
make                  # 产物 tcmalloc
./tcmalloc

# 高并发内存池 —— 单元测试（TLStest）
g++ -o gtcmalloc main.cc -std=c++11
./gtcmalloc

# 高并发内存池 —— 对齐与桶号一致性测试
g++ -o align_test AlignTest.cc -std=c++11
./align_test          # 退出码 0 表示全部通过

# 页映射单元测试（两种实现各跑一遍）
g++ -o page_map_test PageMapTest.cc -std=c++11 && ./page_map_test
g++ -o page_map_test_hash PageMapTest.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0 && ./page_map_test_hash

# 多线程混合尺寸压力测试（两种实现各跑一遍）
g++ -O2 -o stress_test StressTest.cc -std=c++11 && ./stress_test
g++ -O2 -o stress_test_hash StressTest.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0 && ./stress_test_hash

# 定长内存池 —— 与 new/delete 性能对比
cd ../Fixed_length_memory_pool
make                  # 产物 Objectpool
./Objectpool
```

基准测试参数（BenchMark.cc）：4 线程、每轮 1000 次 16 字节申请+释放、共 10 轮，
输出内存池与 `malloc` 各自的耗时（ms）。注意它的计时用 `clock()` 且计时区间会被 `-O2`
优化掉，实测在该优化级别下打印全 0 ms（不加 `-O2` 时约 5–7 ms），因此**目前不能**
用它来对比并发场景下的收益；下面那张表用的是独立探针。

### 页映射替换前后（`steady_clock`，每线程 10 万次，16 字节）

测量方式：临时探针（每线程先预热 10 万次申请/释放，再计时一轮 10 万次申请与一轮 10 万次释放），
取"线程内平均延迟"（各线程耗时之和 ÷ 总操作数），单位 ns/op，`g++ -O2 -std=c++11`。
基线与对照实际使用的编译命令：

```
g++ -O2 -o _perf_radix _perf.cc -std=c++11                              # 基数树（默认 mode 1，不加 -D）
g++ -O2 -o _perf_hash  _perf.cc -std=c++11 -DTC_USE_RADIX_PAGEMAP=0     # 对照
```

| 线程数 | 基数树 alloc | 基数树 free | unordered_map alloc | unordered_map free |
|---|---|---|---|---|
| 1 | 3.1 | 4.7 | 3.3 | 23.9 |
| 2 | 12.4 | 8.6 | 17.6 | 57.0 |
| 4 | 23.6 | 16.6 | 58.4 | 91.7 |
| 8 | 71.8 | 70.7 | 107.2 | 357.7 |

上表是一次代表性运行；同一二进制重复三次的 8 线程 free 区间为 55.8–71.6（基数树）与
357.7–419.1（对照），两者不重叠，结论稳定。

说明：free 一侧的收益来自「不再为每次查找获取 `PageCache::_pageMtx`」；
批量归还仍要拿 CentralCache 的桶锁，因此**不承诺追上 `malloc`**。
alloc 一侧同样受益，但只体现在**多线程**：2 线程 17.6→12.4、4 线程 58.4→23.6、8 线程 107.2→71.8；
1 线程的 3.3→3.1 在噪声范围内，不能算收益。
单线程的收益只在 free 一侧稳定可见（23.9→4.7，约 5 倍）：对照实现每次查找都要走一次互斥锁 + 哈希表探测。

---

## 四、目录结构

```
├── ThreadCachingMalloc/          # 高并发内存池（主体）
│   ├── ConcurrentAlloc.hpp       #   公开接口（TLS 线程缓存入口）
│   ├── ThreadCache.hpp           #   线程缓存（哈希桶 + 慢启动）
│   ├── CentralCache.hpp          #   中心缓存（桶锁）
│   ├── PageCache.hpp             #   页缓存（单例，span 管理/回收合并）
│   ├── PageMap.hpp               #   基数树页号映射（三层，节点惰性分配）
│   ├── ObjectPool.hpp            #   定长对象池（内部对象分配）
│   ├── Common.hpp                #   公共定义（Span/SizeClass/FreeList）
│   ├── UnitTest.hpp              #   单元测试
│   ├── PageMapTest.cc            #   页映射单元测试（独立 main）
│   ├── AlignTest.cc              #   不变量与回归测试（对齐/桶号/span/大块合并/边界）
│   ├── StressTest.cc             #   多线程混合尺寸压力测试（独立 main）
│   ├── BenchMark.cc              #   性能基准（vs malloc）
│   ├── main.cc                   #   测试入口
│   └── Log/                      #   日志模块
└── Fixed_length_memory_pool/     # 定长内存池（前置练习）
    ├── Objectpool.hpp            #   ObjectPool<T> 模板
    ├── main.cc                   #   性能对比用例
    └── Makefile
```

---

## 五、开发记录

按提交历史：初始提交 → ThreadCache → CentralCache → PageCache → 内存回收模块 → 修复 → **基数树页号映射优化**（当前最新）。