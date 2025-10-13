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
#include "cache_meta.h"
#include <atomic>
#include <chrono>
#include <thread>
#include "cache_layout.h"
#include "file/file.h"
#include "logger/logger.h"

namespace UC {

static constexpr uint32_t Magic = (('C' << 16) | ('m' << 8) | 1);

struct Header {
    std::atomic<uint32_t> magic;
    uint32_t padding;
    uint64_t reserved[7];
};

CacheMeta::~CacheMeta()
{
    if (this->addr_) {
        File::MUnmap(this->addr_, this->size_);
        File::ShmUnlink(CacheLayout::MetaShmFile());
    }
    this->addr_ = nullptr;
}

Status CacheMeta::Setup(const Index capacity) noexcept
{
    // CacheMeta包含索引和哈希表
    this->index_.Setup(capacity);   // 分配与释放
    this->hash_.Setup(capacity);    // 插入与查找与删除
    this->size_ = this->index_.MemorySize() + this->hash_.MemorySize();
    // 创建一个文件，用于存储CacheMeta
    auto file = File::Make(CacheLayout::MetaShmFile());
    if (!file) { return Status::OutOfMemory(); }
    // flag 用于判断是否是第一次创建CacheMeta
    auto openFlags = IFile::OpenFlag::CREATE | IFile::OpenFlag::EXCL | IFile::OpenFlag::READ_WRITE;
    auto status = file->ShmOpen(openFlags);
    // 多进程共享的文件，只有一个进程能创建成功
    // 如果是第一次创建CacheMeta，那么需要初始化CacheMeta
    if (status.Success()) { return this->InitShmMeta(file.get()); }
    // 如果是第二次创建CacheMeta，那么需要加载CacheMeta
    if (status == Status::DuplicateKey()) { return this->LoadShmMeta(file.get()); }
    return status;
}

CacheMeta::Index CacheMeta::Alloc(const std::string& id, const size_t offset) noexcept
{
    // 调用CacheIndex的Acquire方法，尝试分配一个槽位
    auto index = this->index_.Acquire();
    if (index != CacheIndex::npos) {
        // index分配成功则在hash表中插入一个节点
        this->hash_.Insert(id, offset, index);
        return index;
    }
    // index没分配成功说明满了，需要淘汰一个槽位
    auto evict = this->hash_.Evict();
    // 淘汰成功把索引表的槽位释放
    if (evict != CacheHash::npos) { this->index_.Release(evict); }
    return npos;
}

CacheMeta::Index CacheMeta::Find(const std::string& id, const size_t offset) noexcept
{
    auto idx = this->hash_.Find(id, offset);
    return idx != CacheHash::npos ? idx : npos;
}

void CacheMeta::PutRef(const uint32_t index) noexcept { this->hash_.PutRef(index); }

void CacheMeta::PutRef(const std::string& id, const size_t offset) noexcept
{
    this->hash_.PutRef(id, offset);
}

Status CacheMeta::InitShmMeta(IFile* shmMetaFile)
{
    // 初始化CacheMeta的文件大小
    auto status = shmMetaFile->Truncate(this->size_);
    if (status.Failure()) { return status; }
    // 把共享内存文件映射到本进程的虚拟地址空间
    status = shmMetaFile->MMap(this->addr_, this->size_, true, true, true);
    if (status.Failure()) { return status; }
    auto header = (Header*)this->addr_;
    // 计算 index 和 hash 的起始地址
    auto indexBase = (void*)(((uint8_t*)this->addr_) + sizeof(Header));
    auto hashBase = (void*)(((uint8_t*)indexBase) + this->index_.MemorySize());
    // 初始化index和hash
    this->index_.Setup(indexBase);
    this->hash_.Setup(hashBase);
    header->padding = 0;
    auto reservedSize = sizeof(header->reserved) / sizeof(*header->reserved);
    std::fill_n(header->reserved, reservedSize, 0);
    // 写入魔数，用于校验是否初始化过
    header->magic = Magic;
    return Status::OK();
}

Status CacheMeta::LoadShmMeta(IFile* shmMetaFile)
{
    // 打开共享内存文件
    auto openFlags = IFile::OpenFlag::READ_WRITE;
    auto status = shmMetaFile->ShmOpen(openFlags);
    if (status.Failure()) { return status; }
    constexpr auto retryInterval = std::chrono::milliseconds(100);
    constexpr auto maxTryTime = 100;
    auto tryTime = 0;
    IFile::FileStat stat;
    // 等待共享内存文件大小与CacheMeta的大小一致，也就是等待某个进程执行truncate文件完成
    do {
        if (tryTime > maxTryTime) {
            UC_ERROR("Shm file({}) not ready.", shmMetaFile->Path());
            return Status::Retry();
        }
        std::this_thread::sleep_for(retryInterval);
        status = shmMetaFile->Stat(stat);
        if (status.Failure()) { return status; }
        tryTime++;
    } while (static_cast<size_t>(stat.st_size) != this->size_);
    // 把共享内存文件映射到本进程的虚拟地址空间
    status = shmMetaFile->MMap(this->addr_, this->size_, true, true, true);
    if (status.Failure()) { return status; }
    auto header = (Header*)this->addr_;
    tryTime = 0;
    // 等待共享内存文件初始化完成，也就是等待某个进程执行InitShmMeta完成
    do {
        if (header->magic == Magic) { break; }
        if (tryTime > maxTryTime) {
            UC_ERROR("Shm file({}) not ready.", shmMetaFile->Path());
            return Status::Retry();
        }
        std::this_thread::sleep_for(retryInterval);
        tryTime++;
    } while (true);
    auto indexBase = (void*)(((uint8_t*)this->addr_) + sizeof(Header));
    auto hashBase = (void*)(((uint8_t*)indexBase) + this->index_.MemorySize());
    // 初始化index和hash，拿到地址空间
    this->index_.Setup(indexBase);
    this->hash_.Setup(hashBase);
    return Status::OK();
}

} // namespace UC
