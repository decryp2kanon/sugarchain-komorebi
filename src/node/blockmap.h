// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_NODE_BLOCKMAP_H
#define BITCOIN_NODE_BLOCKMAP_H

#include <chain.h>
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
#include <variant>
#include <vector>

namespace node {
/** Stable-address block indexes. The compact opt-in uses 32-bit hash slots
 * pointing into append-only chunks, avoiding per-node links and pointer-sized
 * buckets. Live entries and their hash keys never move during growth/rehash.
 * The facade below selects the separate original std::unordered_map backend.
 */
class CompactBlockMap {
public:
    using value_type = std::pair<const uint256, CBlockIndex>;
private:
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
        friend class CompactBlockMap;
        template <bool> friend class Iterator;
        using Owner = std::conditional_t<Constant, const CompactBlockMap, CompactBlockMap>;
        Owner* m_owner{nullptr};
        size_t m_id{0};
        Iterator(Owner* owner, size_t id) : m_owner{owner}, m_id{id} { Skip(); }
        void Skip() { while (m_id < m_owner->m_next && !m_owner->m_live[m_id]) ++m_id; }
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = CompactBlockMap::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Constant, const value_type&, value_type&>;
        using pointer = std::conditional_t<Constant, const value_type*, value_type*>;
        Iterator() = default;
        template <bool Other> requires (Constant && !Other)
        Iterator(const Iterator<Other>& other) : m_owner{other.m_owner}, m_id{other.m_id} {}
        reference operator*() const { return *m_owner->Entry(m_id); }
        pointer operator->() const { return &**this; }
        Iterator& operator++()
        {
            ++m_id; Skip();
            return *this;
        }
        Iterator operator++(int) { auto previous{*this}; ++*this; return previous; }
        bool operator==(const Iterator& other) const
        {
            if (m_owner != other.m_owner) return false;
            if (!m_owner) return true;
            return m_id == other.m_id;
        }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    CompactBlockMap() = default;
    ~CompactBlockMap()
    {
        for (size_t id = 0; id < m_next; ++id) if (m_live[id]) std::destroy_at(Entry(id));
    }
    CompactBlockMap(const CompactBlockMap&) = delete;
    CompactBlockMap& operator=(const CompactBlockMap&) = delete;
    size_t size() const { return m_size; }
    bool empty() const { return size() == 0; }
    size_t max_size() const { return std::min<size_t>(TOMBSTONE - 1, std::numeric_limits<size_t>::max() / sizeof(value_type) / 2); }
    size_t bucket_count() const { return m_slots.size(); }
    float max_load_factor() const { return 0.75F; }
    iterator begin() { return iterator{this, size_t{0}}; }
    iterator end() { return iterator{this, m_next}; }
    const_iterator begin() const { return const_iterator{this, size_t{0}}; }
    const_iterator end() const { return const_iterator{this, m_next}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator find(const uint256& key)
    {
        const size_t bucket{Locate(key, m_hasher(key))};
        return m_slots.empty() || m_slots[bucket].Id() == 0 ? end() : iterator{this, size_t{m_slots[bucket].Id() - 1}};
    }
    const_iterator find(const uint256& key) const
    {
        const size_t bucket{Locate(key, m_hasher(key))};
        return m_slots.empty() || m_slots[bucket].Id() == 0 ? end() : const_iterator{this, size_t{m_slots[bucket].Id() - 1}};
    }
    bool contains(const uint256& key) const { return find(key) != end(); }
    size_t count(const uint256& key) const { return contains(key) ? 1 : 0; }
    CBlockIndex& operator[](const uint256& key) { return try_emplace(key).first->second; }
    void reserve(size_t count)
    {
        if (count > max_size()) throw std::length_error{"Compact block index capacity exceeded"};
        const size_t needed{std::max<size_t>(16, count + (count + 2) / 3)};
        const size_t capacity{std::bit_ceil(needed)};
        if (capacity > m_slots.size()) Rehash(capacity);
    }
    template <class... Args>
    std::pair<iterator, bool> try_emplace(const uint256& key, Args&&... args)
    {
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

/** Select one backend before loading any indexes. Mode zero instantiates the
 * exact pre-PR #10 map type, including its default allocator and hash caching.
 * Compact metadata and its salted hasher are constructed only in mode one.
 * This facade adds constant mode dispatch, not an optimized legacy backend.
 */
class BlockMap {
public:
    using Legacy = std::unordered_map<uint256, CBlockIndex, BlockHasher>;
    using value_type = Legacy::value_type;
private:
    std::variant<Legacy, std::unique_ptr<CompactBlockMap>> m_backend;

    template <class F> decltype(auto) Visit(F&& function)
    {
        return std::visit([&](auto& backend) -> decltype(auto) {
            if constexpr (std::is_same_v<std::decay_t<decltype(backend)>, Legacy>) return function(backend);
            else return function(*backend);
        }, m_backend);
    }
    template <class F> decltype(auto) Visit(F&& function) const
    {
        return std::visit([&](const auto& backend) -> decltype(auto) {
            if constexpr (std::is_same_v<std::decay_t<decltype(backend)>, Legacy>) return function(backend);
            else return function(std::as_const(*backend));
        }, m_backend);
    }
public:
    template <bool Constant> class Iterator {
        friend class BlockMap;
        template <bool> friend class Iterator;
        using Owner = std::conditional_t<Constant, const BlockMap, BlockMap>;
        using LegacyIterator = std::conditional_t<Constant, Legacy::const_iterator, Legacy::iterator>;
        using CompactIterator = std::conditional_t<Constant, CompactBlockMap::const_iterator, CompactBlockMap::iterator>;
        Owner* m_owner{nullptr};
        std::variant<LegacyIterator, CompactIterator> m_iterator;
        Iterator(Owner* owner, LegacyIterator it) : m_owner{owner}, m_iterator{std::in_place_index<0>, it} {}
        Iterator(Owner* owner, CompactIterator it) : m_owner{owner}, m_iterator{std::in_place_index<1>, it} {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = BlockMap::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Constant, const value_type&, value_type&>;
        using pointer = std::conditional_t<Constant, const value_type*, value_type*>;
        Iterator() = default;
        template <bool Other> requires (Constant && !Other)
        Iterator(const Iterator<Other>& other) : m_owner{other.m_owner}
        {
            if (other.m_iterator.index() == 0) m_iterator.template emplace<0>(std::get<0>(other.m_iterator));
            else m_iterator.template emplace<1>(std::get<1>(other.m_iterator));
        }
        reference operator*() const { return std::visit([](const auto& it) -> reference { return *it; }, m_iterator); }
        pointer operator->() const { return &**this; }
        Iterator& operator++() { std::visit([](auto& it) { ++it; }, m_iterator); return *this; }
        Iterator operator++(int) { auto previous{*this}; ++*this; return previous; }
        bool operator==(const Iterator& other) const
        {
            if (m_owner != other.m_owner || m_iterator.index() != other.m_iterator.index()) return false;
            if (!m_owner) return true;
            return m_iterator.index() == 0 ? std::get<0>(m_iterator) == std::get<0>(other.m_iterator) :
                std::get<1>(m_iterator) == std::get<1>(other.m_iterator);
        }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    explicit BlockMap(bool compact = false)
    {
        if (compact) m_backend.emplace<1>(std::make_unique<CompactBlockMap>());
    }
    BlockMap(const BlockMap&) = delete;
    BlockMap& operator=(const BlockMap&) = delete;
    bool IsCompact() const { return m_backend.index() == 1; }
    size_t size() const { return Visit([](const auto& map) { return map.size(); }); }
    bool empty() const { return size() == 0; }
    size_t max_size() const { return Visit([](const auto& map) { return map.max_size(); }); }
    size_t bucket_count() const { return Visit([](const auto& map) { return map.bucket_count(); }); }
    float max_load_factor() const { return Visit([](const auto& map) { return map.max_load_factor(); }); }
    iterator begin() { return Visit([&](auto& map) { return iterator{this, map.begin()}; }); }
    iterator end() { return Visit([&](auto& map) { return iterator{this, map.end()}; }); }
    const_iterator begin() const { return Visit([&](const auto& map) { return const_iterator{this, map.begin()}; }); }
    const_iterator end() const { return Visit([&](const auto& map) { return const_iterator{this, map.end()}; }); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator find(const uint256& key) { return Visit([&](auto& map) { return iterator{this, map.find(key)}; }); }
    const_iterator find(const uint256& key) const { return Visit([&](const auto& map) { return const_iterator{this, map.find(key)}; }); }
    bool contains(const uint256& key) const { return Visit([&](const auto& map) { return map.contains(key); }); }
    size_t count(const uint256& key) const { return contains(key) ? 1 : 0; }
    CBlockIndex& operator[](const uint256& key) { return try_emplace(key).first->second; }
    void reserve(size_t count) { Visit([&](auto& map) { map.reserve(count); }); }
    template <class... Args> std::pair<iterator, bool> try_emplace(const uint256& key, Args&&... args)
    {
        return Visit([&](auto& map) {
            auto [it, inserted]{map.try_emplace(key, std::forward<Args>(args)...)};
            return std::pair<iterator, bool>{iterator{this, it}, inserted};
        });
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
        if (IsCompact()) return iterator{this, std::get<1>(m_backend)->erase(std::get<1>(it.m_iterator))};
        return iterator{this, std::get<0>(m_backend).erase(std::get<0>(it.m_iterator))};
    }
};
} // namespace node
#endif // BITCOIN_NODE_BLOCKMAP_H
