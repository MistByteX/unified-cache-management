/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#include "cache_index.h"
#include <algorithm>
#include <atomic>

namespace UC {

struct Node {
    uint32_t idx;   // 节点的索引，0 ~ capacity-1
    uint32_t next;  // 指向下一个节点的索引，0表示无下一个节点
};
struct Pointer {
    uint32_t slot;  // 指向当前头节点的索引
    uint32_t ver;   // 版本号
};
struct Header {
    uint32_t magic; // 魔数，多进程共享内存，用于校验
    uint32_t capacity;  // 容量
    alignas(64) std::atomic<Pointer> pointer;   // 指向节点的指针，用alignas避免cacheline伪共享
    uint64_t padding[7];    // 填充
    Node nodes[0];  // 节点数组，索引从1开始，0表示无节点
};
static_assert(sizeof(Pointer) == 8, "Pointer must be 64-bit");
static_assert(sizeof(Header) == 128, "Header must be 128-Byte");

static constexpr uint32_t Magic = (('C' << 16) | ('i' << 8) | 1);
inline auto HeaderPtr(void* addr) { return (Header*)addr; }

size_t CacheIndex::MemorySize() const noexcept
{
    return sizeof(Header) + sizeof(Node) * (this->capacity_ + 1);
}

void CacheIndex::Setup(void* addr) noexcept
{
    // 拿到分配的地址空间，将其转换成头部指针
    this->addr_ = addr;
    auto header = HeaderPtr(this->addr_);
    // 如果魔数相等，说明已经初始化过了，直接返回
    if (header->magic == Magic) { return; }
    header->capacity = this->capacity_;
    // 初始化指针，指向节点1，版本号为0
    // 0号节点代表空节点，用来标记为空
    header->pointer.store({1, 0});
    // 为了避免伪共享，将pointer和node分开，将pointer单独占一个cacheline
    // 因为cacheline是64字节，pointer只有8个字节，需要填充56个字节
    auto paddingSize = sizeof(header->padding) / sizeof(*header->padding);
    std::fill_n(header->padding, paddingSize, 0);
    // 初始化节点数组
    // 1号节点到capacity号节点分别对应0到capacity-1的索引值
    // 每个节点的next指向槽位编号+1，最后一个节点的next指向0
    for (uint32_t slot = 1; slot <= header->capacity; slot++) {
        header->nodes[slot].idx = slot - 1;
        header->nodes[slot].next = slot + 1;
    }
    header->nodes[header->capacity].next = 0;
    // 初始化完成后，将魔数设置为Magic， 用于校验
    header->magic = Magic;
    return;
}

uint32_t CacheIndex::Acquire() noexcept
{
    auto header = HeaderPtr(this->addr_);
    for (;;) {
        // 获取当前头节点指针，判断是否为空
        auto ptr = header->pointer.load(std::memory_order_acquire);
        if (ptr.slot == 0) { return npos; }
        // 获取当前指针的下一个节点
        auto next = header->nodes[ptr.slot].next;
        // 用cas更新将当前指针的指向更新为指向下一个槽位
        Pointer desired{next, ptr.ver + 1};
        if (header->pointer.compare_exchange_weak(ptr, desired, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
            // 更新成功将当前指针的索引值返回
            return header->nodes[ptr.slot].idx;
        }
    }
}

void CacheIndex::Release(const uint32_t idx) noexcept
{
    auto header = HeaderPtr(this->addr_);
    if (idx >= header->capacity) { return; }
    // 根据索引号获取到槽位号
    auto slot = idx + 1;
    for (;;) {
        // 获取当前指针
        auto ptr = header->pointer.load(std::memory_order_acquire);
        // 将归还节点的槽位的next指向在头部的节点，也就是插入到头部
        header->nodes[slot].next = ptr.slot;
        // 用cas更新将当前pointer的指向更新为指向归还的节点
        Pointer desired{slot, ptr.ver + 1};
        if (header->pointer.compare_exchange_weak(ptr, desired, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
            return;
        }
    }
}

} // namespace UC

/*
为什么要有pointer的ver版本号？
如果只有slot，而没有ver版本号，那么在cas更新当前指针时，可能会出现以下情况：

例如:pointer->1->2->3->0

1号线程：获取ptr=1，next=2

2号线程：获取ptr=1，next=2，将pointer指向2

2号线程：获取ptr=2，next=3，将pointer指向3

2号线程：归还节点1，将节点1的next指向当前槽位3，pointer=1

1号线程：在1号线程来看，slot没有变化，此时用cas，当前指针会指向2而不是3
*/
