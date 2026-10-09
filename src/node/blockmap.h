// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_NODE_BLOCKMAP_H
#define BITCOIN_NODE_BLOCKMAP_H

#include <chain.h>
#include <support/allocators/pool.h>
#include <util/hasher.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace node {
template <class T>
class BlockIndexAllocator {
    template <class U> friend class BlockIndexAllocator;
    PoolResource<512, 8>* m_resource;
public:
    using value_type = T;
    explicit BlockIndexAllocator(PoolResource<512, 8>* resource = nullptr) noexcept : m_resource{resource} {}
    template <class U>
    BlockIndexAllocator(const BlockIndexAllocator<U>& other) noexcept : m_resource{other.m_resource} {}
    template <class U> struct rebind { using other = BlockIndexAllocator<U>; };
    T* allocate(size_t count)
    {
        if (m_resource) return static_cast<T*>(m_resource->Allocate(count * sizeof(T), alignof(T)));
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* ptr, size_t count) noexcept
    {
        if (m_resource) m_resource->Deallocate(ptr, count * sizeof(T), alignof(T));
        else std::allocator<T>{}.deallocate(ptr, count);
    }
    template <class U>
    bool operator==(const BlockIndexAllocator<U>& other) const noexcept { return m_resource == other.m_resource; }
};

/** Stable-address block indexes. The compact opt-in uses 32-bit hash slots
 * pointing into append-only chunks, avoiding per-node links and pointer-sized
 * buckets. Live entries and their hash keys never move during growth/rehash.
 * The legacy implementation remains std::unordered_map.
 */
class BlockMap {
public:
    using value_type = std::pair<const uint256, CBlockIndex>;
    using allocator_type = BlockIndexAllocator<value_type>;
private:
    using Legacy = std::unordered_map<uint256, CBlockIndex, BlockHasher, std::equal_to<uint256>, allocator_type>;
    static constexpr uint32_t TOMBSTONE = std::numeric_limits<uint32_t>::max();
    static constexpr size_t CHUNK_ENTRIES = 4096;
    struct alignas(value_type) EntrySlot { std::byte bytes[sizeof(value_type)]; };
    static_assert(sizeof(EntrySlot) == sizeof(value_type));
    // Keep a short fingerprint beside each ID. A mismatch avoids a scattered
    // full-key read; a match still requires exact uint256 equality. Byte storage
    // makes the five-byte slots portable without unaligned typed accesses.
    struct HashSlot {
        std::array<unsigned char, 5> bytes{};
        uint32_t Id() const { return ReadLE32(bytes.data()); }
        uint8_t Tag() const { return bytes[4]; }
        void Set(uint32_t id, uint8_t tag) { WriteLE32(bytes.data(), id); bytes[4] = tag; }
    };
    static_assert(sizeof(HashSlot) == 5);
    static uint8_t HashTag(size_t hash) { return hash >> (sizeof(size_t) * 8 - 8); }
    Legacy m_legacy;
    const bool m_compact;
    SaltedUint256Hasher m_hasher;
    std::vector<std::unique_ptr<EntrySlot[]>> m_chunks;
    std::vector<bool> m_live;
    std::vector<HashSlot> m_slots;
    size_t m_next{0};
    size_t m_size{0};
    size_t m_deleted{0};
    uint32_t m_free_head{0};

    unsigned char* Bytes(size_t id) const
    {
        return reinterpret_cast<unsigned char*>(m_chunks[id / CHUNK_ENTRIES][id % CHUNK_ENTRIES].bytes);
    }

    value_type* Storage(size_t id) const
    {
        return reinterpret_cast<value_type*>(&m_chunks[id / CHUNK_ENTRIES][id % CHUNK_ENTRIES]);
    }
    value_type* Entry(size_t id) const { return std::launder(Storage(id)); }
    size_t Locate(const uint256& key, size_t hash) const
    {
        if (m_slots.empty()) return 0;
        size_t bucket{hash & (m_slots.size() - 1)};
        const uint8_t tag{HashTag(hash)};
        while (m_slots[bucket].Id() != 0) {
            const uint32_t slot{m_slots[bucket].Id()};
            if (slot != TOMBSTONE && m_slots[bucket].Tag() == tag && Entry(slot - 1)->first == key) return bucket;
            bucket = (bucket + 1) & (m_slots.size() - 1);
        }
        return bucket;
    }
    void Rehash(size_t capacity)
    {
        std::vector<HashSlot> replacement(capacity);
        for (size_t id = 0; id < m_next; ++id) {
            if (!m_live[id]) continue;
            const size_t hash{m_hasher(Entry(id)->first)};
            size_t bucket{hash & (capacity - 1)};
            while (replacement[bucket].Id()) bucket = (bucket + 1) & (capacity - 1);
            replacement[bucket].Set(static_cast<uint32_t>(id + 1), HashTag(hash));
        }
        m_slots.swap(replacement);
        m_deleted = 0;
    }
    bool EnsureInsertCapacity()
    {
        if (m_free_head == 0 && m_next >= max_size()) throw std::length_error{"Compact block index capacity exceeded"};
        bool changed{false};
        if (m_slots.empty()) { Rehash(16); changed = true; }
        if (m_size + m_deleted + 1 > m_slots.size() - m_slots.size() / 4) {
            Rehash(m_size + 1 > m_slots.size() / 2 ? m_slots.size() * 2 : m_slots.size());
            changed = true;
        }
        return changed;
    }

public:
    template <bool Constant> class Iterator {
        friend class BlockMap;
        template <bool> friend class Iterator;
        using Owner = std::conditional_t<Constant, const BlockMap, BlockMap>;
        using LegacyIterator = std::conditional_t<Constant, Legacy::const_iterator, Legacy::iterator>;
        Owner* m_owner{nullptr};
        LegacyIterator m_iterator{};
        size_t m_id{0};
        Iterator(Owner* owner, size_t id) : m_owner{owner}, m_id{id} { Skip(); }
        Iterator(Owner* owner, LegacyIterator it) : m_owner{owner}, m_iterator{it} {}
        void Skip() { while (m_id < m_owner->m_next && !m_owner->m_live[m_id]) ++m_id; }
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = BlockMap::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Constant, const value_type&, value_type&>;
        using pointer = std::conditional_t<Constant, const value_type*, value_type*>;
        Iterator() = default;
        template <bool Other> requires (Constant && !Other)
        Iterator(const Iterator<Other>& other) : m_owner{other.m_owner}, m_iterator{other.m_iterator}, m_id{other.m_id} {}
        reference operator*() const { return m_owner->m_compact ? *m_owner->Entry(m_id) : *m_iterator; }
        pointer operator->() const { return &**this; }
        Iterator& operator++()
        {
            if (m_owner->m_compact) { ++m_id; Skip(); } else ++m_iterator;
            return *this;
        }
        Iterator operator++(int) { auto previous{*this}; ++*this; return previous; }
        bool operator==(const Iterator& other) const
        {
            if (m_owner != other.m_owner) return false;
            if (!m_owner) return true;
            return m_owner->m_compact ? m_id == other.m_id : m_iterator == other.m_iterator;
        }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    BlockMap(size_t count = 0, BlockHasher hash = {}, std::equal_to<uint256> equal = {},
             allocator_type allocator = allocator_type{}, bool compact = false)
        : m_legacy{count, hash, equal, allocator}, m_compact{compact} {}
    ~BlockMap()
    {
        if (m_compact) for (size_t id = 0; id < m_next; ++id) if (m_live[id]) std::destroy_at(Entry(id));
    }
    BlockMap(const BlockMap&) = delete;
    BlockMap& operator=(const BlockMap&) = delete;
    bool IsCompact() const { return m_compact; }
    size_t size() const { return m_compact ? m_size : m_legacy.size(); }
    bool empty() const { return size() == 0; }
    size_t max_size() const { return m_compact ? std::min<size_t>(TOMBSTONE - 1, std::numeric_limits<size_t>::max() / sizeof(value_type) / 2) : m_legacy.max_size(); }
    size_t bucket_count() const { return m_compact ? m_slots.size() : m_legacy.bucket_count(); }
    float max_load_factor() const { return m_compact ? 0.75F : m_legacy.max_load_factor(); }
    iterator begin() { return m_compact ? iterator{this, size_t{0}} : iterator{this, m_legacy.begin()}; }
    iterator end() { return m_compact ? iterator{this, m_next} : iterator{this, m_legacy.end()}; }
    const_iterator begin() const { return m_compact ? const_iterator{this, size_t{0}} : const_iterator{this, m_legacy.begin()}; }
    const_iterator end() const { return m_compact ? const_iterator{this, m_next} : const_iterator{this, m_legacy.end()}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator find(const uint256& key)
    {
        if (!m_compact) return iterator{this, m_legacy.find(key)};
        const size_t bucket{Locate(key, m_hasher(key))};
        return m_slots.empty() || m_slots[bucket].Id() == 0 ? end() : iterator{this, size_t{m_slots[bucket].Id() - 1}};
    }
    const_iterator find(const uint256& key) const
    {
        if (!m_compact) return const_iterator{this, m_legacy.find(key)};
        const size_t bucket{Locate(key, m_hasher(key))};
        return m_slots.empty() || m_slots[bucket].Id() == 0 ? end() : const_iterator{this, size_t{m_slots[bucket].Id() - 1}};
    }
    bool contains(const uint256& key) const { return find(key) != end(); }
    size_t count(const uint256& key) const { return contains(key) ? 1 : 0; }
    CBlockIndex& operator[](const uint256& key) { return try_emplace(key).first->second; }
    void reserve(size_t count)
    {
        if (!m_compact) { m_legacy.reserve(count); return; }
        if (count > max_size()) throw std::length_error{"Compact block index capacity exceeded"};
        const size_t needed{std::max<size_t>(16, count + (count + 2) / 3)};
        const size_t capacity{std::bit_ceil(needed)};
        if (capacity > m_slots.size()) Rehash(capacity);
    }
    template <class... Args>
    std::pair<iterator, bool> try_emplace(const uint256& key, Args&&... args)
    {
        if (!m_compact) {
            auto [it, inserted]{m_legacy.try_emplace(key, std::forward<Args>(args)...)};
            return {iterator{this, it}, inserted};
        }
        const size_t hash{m_hasher(key)};
        size_t bucket{Locate(key, hash)};
        if (!m_slots.empty() && m_slots[bucket].Id() != 0) {
            return {iterator{this, size_t{m_slots[bucket].Id() - 1}}, false};
        }
        if (EnsureInsertCapacity()) bucket = Locate(key, hash);
        const bool reused{m_free_head != 0};
        const size_t id{reused ? m_free_head - 1 : m_next};
        const uint32_t next_free{reused ? ReadLE32(Bytes(id)) : 0};
        if (id / CHUNK_ENTRIES == m_chunks.size()) m_chunks.push_back(std::make_unique_for_overwrite<EntrySlot[]>(CHUNK_ENTRIES));
        if (!reused) m_live.resize(m_next + 1, false);
        try {
            std::construct_at(Storage(id), std::piecewise_construct,
                              std::forward_as_tuple(key), std::forward_as_tuple(std::forward<Args>(args)...));
        } catch (...) {
            if (reused) WriteLE32(Bytes(id), next_free);
            throw;
        }
        if (reused) m_free_head = next_free; else ++m_next;
        m_live[id] = true;
        m_slots[bucket].Set(static_cast<uint32_t>(id + 1), HashTag(hash));
        ++m_size;
        return {iterator{this, id}, true};
    }
    template <class KeyTuple, class ValueTuple>
    std::pair<iterator, bool> emplace(std::piecewise_construct_t, KeyTuple&& keys, ValueTuple&& values)
    {
        return std::apply([&](auto&&... args) {
            return try_emplace(std::get<0>(std::forward<KeyTuple>(keys)), std::forward<decltype(args)>(args)...);
        }, std::forward<ValueTuple>(values));
    }
    iterator erase(iterator it)
    {
        if (!m_compact) return iterator{this, m_legacy.erase(it.m_iterator)};
        const size_t bucket{Locate(it->first, m_hasher(it->first))};
        auto next{it}; ++next;
        std::destroy_at(Entry(it.m_id));
        // Reuse dead storage without allocating a free-list under pressure.
        // No live key, index or pointer changes during this operation.
        WriteLE32(Bytes(it.m_id), m_free_head);
        m_free_head = static_cast<uint32_t>(it.m_id + 1);
        m_live[it.m_id] = false;
        m_slots[bucket].Set(TOMBSTONE, 0);
        --m_size;
        ++m_deleted;
        return next;
    }
};
} // namespace node
#endif // BITCOIN_NODE_BLOCKMAP_H
