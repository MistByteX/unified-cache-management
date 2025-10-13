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
#include "cache_hash.h"
#include <atomic>
#include <pthread.h>

namespace UC {

static constexpr uint32_t BucketNumber = 17683;
static constexpr uint32_t Magic = (('C' << 16) | ('h' << 8) | 1);

struct Key {
    // id : k1, k2
    // k1, k2, off 组合成一个 hash 值，作为在 hash 表中的索引(哪个bucket)
    uint64_t k1, k2;
    size_t off;
    Key() { this->Init(); }
    Key(const std::string& id, const size_t off) { this->Fill(id, off); }
    bool operator==(const Key& k) const { return k1 == k.k1 && k2 == k.k2 && off == k.off; }
    void Init() { this->k1 = this->k2 = this->off = 0; }
    // 将id和off填充到key的各个部分
    void Fill(const std::string& id, const size_t off)
    {
        auto idPair = static_cast<const uint64_t*>(static_cast<const void*>(id.data()));
        this->k1 = idPair[0];
        this->k2 = idPair[1];
        this->off = off;
    }
    // 将k1和k2和off组成一个对于bucket的hash值
    uint32_t Hash()
    {
        static std::hash<size_t> hasher{};
        return (hasher(k1) | hasher(k2) | hasher(off)) % BucketNumber;
    }
};

struct Node {
    Key key;    // 每个Node的key，也就是这个节点属于哪个bucket
    uint32_t prev;  // 前一个节点的索引
    uint32_t next;  // 后一个节点的索引
    std::atomic<uint64_t> ref;  // 引用计数，读取的时候++
    uint64_t tp;    // 时间戳，用于LRU算法(header的tp++完成赋值过来的)
    void Init()
    {
        this->key.Init();
        this->prev = CacheHash::npos;
        this->next = CacheHash::npos;
        this->ref = 0;
    }
};

struct Bucket {
    pthread_mutex_t mutex;  // 每个bucket都有一个mutex，用于保护对这个bucket的操作
    uint32_t head;  // 这个bucket的头节点索引
    uint32_t tail;  // 这个bucket的尾节点索引
    void Init()
    {
        // 共享锁（跨进程），所有进程的所有线程间互斥
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&this->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        this->head = CacheHash::npos;
        this->tail = CacheHash::npos;
    }
    void Lock() { pthread_mutex_lock(&this->mutex); }
    void Unlock() { pthread_mutex_unlock(&this->mutex); }
};

struct Header {
    uint32_t magic;  // 魔法数，用于验证shm文件是否正确加载
    uint32_t capacity;  // 缓存的最大容量
    std::atomic<uint64_t> tp;  // 时间戳，用于LRU算法(首次insert和find操作++，然后赋值给Node.tp)
    uint64_t reserved[7];   // tp前面两个变量正好占一个cacheline，所以tp不用alignas，直接后面填充
    Bucket buckets[BucketNumber];   // 桶
    Node nodes[0];  // 所有节点
};

inline auto HeaderPtr(void* addr) { return (Header*)addr; }

inline void InsertNode2Bucket(Header* header, Bucket* bucket, const uint32_t index)
{
    auto slot = header->nodes + index;  // 根据cache_index预分配的index，拿到对应的node
    slot->prev = CacheHash::npos;  // 新节点插入到最前面没有前驱，prev指向npos
    slot->next = bucket->head;  // 头插法，新节点的next指向原来的头节点
    // 如果bucket原来有头节点，原来头节点的前驱就是新节点
    if (bucket->head != CacheHash::npos) {
        auto next = header->nodes + bucket->head;
        next->prev = index;
    }
    // 现在bucket的头要指向新节点
    bucket->head = index;
    // 如果bucket的尾节点不是空的，尾节点指向新节点(因为是头插法，只有空的时候才需要指向，剩下的都会插到表头)
    if (bucket->tail == CacheHash::npos) { bucket->tail = index; }
}

inline void MoveNode2BucketHead(Header* header, Bucket* bucket, const uint32_t index)
{
    // 如果bucket的head就是头节点，直接返回
    if (bucket->head == index) { return; }
    auto slot = header->nodes + index;
    if (bucket->tail == index) {
        // 如果bucket的tail是尾节点
        // 将当前节点的上一个节点作为尾节点
        auto tail = header->nodes + slot->prev;
        tail->next = CacheHash::npos;
        bucket->tail = slot->prev;
        // 当前节点因为要放到头去，所以前驱是null
        slot->prev = CacheHash::npos;
    } else {
        // 如果当前节点在中间，需要将当前节点从中间断开
        auto prev = header->nodes + slot->prev;
        auto next = header->nodes + slot->next;
        prev->next = slot->next;
        next->prev = slot->prev;
    }
    // 拿到当前头节点，将头节点的前驱指向当前节点，当前节点的next指向头节点，bucket的head标记成当前节点
    auto head = header->nodes + bucket->head;
    head->prev = index;
    slot->next = bucket->head;
    bucket->head = index;
}

inline void RemoveNodeFromBucket(Header* header, Bucket* bucket, const uint32_t index)
{
    auto slot = header->nodes + index;
    // 如下被move的节点都不需要置位prev和next，因为重新插入的时候会重新设置
    // 如果只有一个节点，直接将head和tail都设为npos
    if (bucket->head == index && bucket->tail == index) {
        bucket->head = bucket->tail = CacheHash::npos;
        return;
    }
    // 如果节点在中间，需要将当前节点从中间断开
    if (bucket->head != index && bucket->tail != index) {
        auto prev = header->nodes + slot->prev;
        auto next = header->nodes + slot->next;
        prev->next = slot->next;
        next->prev = slot->prev;
        return;
    }
    // 如果在头部，需要将bucket的head指向下一个节点
    if (bucket->head == index) {
        bucket->head = slot->next;
        auto head = header->nodes + slot->next;
        head->prev = CacheHash::npos;
        return;
    }
    // 如果在尾部，需要将bucket的tail指向上一个节点
    if (bucket->tail == index) {
        bucket->tail = slot->prev;
        auto tail = header->nodes + slot->prev;
        tail->next = CacheHash::npos;
        return;
    }
}

size_t CacheHash::MemorySize() const noexcept
{
    // 应该是+吧？
    return sizeof(Header) * sizeof(Node) * this->capacity_;
}

void CacheHash::Setup(void* addr) noexcept
{
    this->addr_ = addr;
    auto header = HeaderPtr(this->addr_);
    if (header->magic == Magic) { return; }
    header->capacity = this->capacity_;
    header->tp.store(0, std::memory_order_relaxed);
    auto reservedSize = sizeof(header->reserved) / sizeof(*header->reserved);
    std::fill_n(header->reserved, reservedSize, 0);
    for (uint32_t i = 0; i < BucketNumber; i++) { header->buckets[i].Init(); }
    for (uint32_t i = 0; i < header->capacity; i++) { header->nodes[i].Init(); }
    header->magic = Magic;
    return;
}

void CacheHash::Insert(const std::string& id, const size_t offset, const uint32_t index) noexcept
{
    auto header = HeaderPtr(this->addr_);
    // 根据cache_index预分配的index，拿到对应的node并且填充key
    auto slot = header->nodes + index;
    slot->key.Fill(id, offset);
    // 根据key的hash值，拿到对应的bucket
    auto bucket = header->buckets + slot->key.Hash();
    bucket->Lock();
    // 节点引用计数初始化
    slot->ref.store(1, std::memory_order_relaxed);
    slot->tp = header->tp.fetch_add(1);
    // 插入节点到桶里
    InsertNode2Bucket(header, bucket, index);
    bucket->Unlock();
}

uint32_t CacheHash::Find(const std::string& id, const size_t offset) noexcept
{
    Key key{id, offset};
    auto header = HeaderPtr(this->addr_);
    auto bucket = header->buckets + key.Hash();
    bucket->Lock();
    auto pos = bucket->head;
    // 在桶中从头遍历直到找到key相等的节点，并且移动到桶头去，返回节点index
    while (pos != npos) {
        auto slot = header->nodes + pos;
        if (slot->key == key) {
            slot->ref++;
            slot->tp = header->tp.fetch_add(1);
            MoveNode2BucketHead(header, bucket, pos);
            break;
        }
        pos = slot->next;
    }
    bucket->Unlock();
    return pos;
}

void CacheHash::PutRef(const uint32_t index) noexcept
{
    // 根据index拿到节点并将节点的ref--
    auto header = HeaderPtr(this->addr_);
    if (index >= header->capacity) { return; }
    auto slot = header->nodes + index;
    auto ref = slot->ref.load(std::memory_order_acquire);
    while (ref > 0) {
        auto desired = ref - 1;
        if (slot->ref.compare_exchange_weak(ref, desired, std::memory_order_acq_rel)) { break; }
        ref = slot->ref.load(std::memory_order_acquire);
    }
}

void CacheHash::PutRef(const std::string& id, const size_t offset) noexcept
{
    // 根据id和offset找到节点并将节点的ref--
    Key key{id, offset};
    auto header = HeaderPtr(this->addr_);
    auto bucket = header->buckets + key.Hash();
    bucket->Lock();
    auto pos = bucket->head;
    while (pos != npos) {
        auto slot = header->nodes + pos;
        if (slot->key == key) {
            this->PutRef(pos);
            break;
        }
        pos = slot->next;
    }
    bucket->Unlock();
}

void CacheHash::Remove(const std::string& id, const size_t offset) noexcept
{
    Key key{id, offset};
    auto header = HeaderPtr(this->addr_);
    auto bucket = header->buckets + key.Hash();
    bucket->Lock();
    auto pos = bucket->head;
    // 找到节点并且remove掉
    while (pos != npos) {
        auto slot = header->nodes + pos;
        if (slot->key == key) {
            RemoveNodeFromBucket(header, bucket, pos);
            break;
        }
        pos = slot->next;
    }
    bucket->Unlock();
}

uint32_t CacheHash::Evict() noexcept
{
    auto header = HeaderPtr(this->addr_);
    auto iBucket = npos;    // 记录选中桶的index
    auto pos = npos;    // 记录选中节点的index
    auto tp = (uint64_t)(-1);   // 记录选中节点的tp值
    // 遍历每个桶，拿到tail的节点比较(因为最新的肯定都插到头去了)
    for (uint32_t i = 0; i < BucketNumber; i++) {
        auto bucket = header->buckets + i;
        bucket->Lock();
        if (bucket->tail != npos) {
            auto slot = header->nodes + bucket->tail;
            // 找到最小的tp且ref==0(没人用)的节点
            if (slot->ref == 0 && tp > slot->tp) {
                iBucket = i;
                pos = bucket->tail;
                tp = slot->tp;
            }
        }
        bucket->Unlock();
    }
    if (iBucket == npos) { return npos; }
    auto bucket = header->buckets + iBucket;
    bucket->Lock();
    auto slot = header->nodes + pos;
    // 再次检查有没有被修改，没有的话就remove掉
    if (bucket->tail != pos || slot->ref != 0) {
        pos = npos;
    } else {
        RemoveNodeFromBucket(header, bucket, pos);
    }
    bucket->Unlock();
    return pos;
}

} // namespace UC
