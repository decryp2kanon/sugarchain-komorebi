// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_NODE_BLOCKMAP_H
#define BITCOIN_NODE_BLOCKMAP_H

#include <chain.h>
#include <support/allocators/pool.h>
#include <util/hasher.h>
#include <iterator>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace node {
// Same hash value as BlockHasher. Keep the node-layout optimization local
// instead of changing the global cached-hash policy.
struct FastBlockHasher {
    size_t operator()(const uint256& hash) const noexcept { return ReadLE64(hash.begin()); }
};
using FastBlockMap = std::unordered_map<uint256, CBlockIndex, FastBlockHasher,
    std::equal_to<uint256>, PoolAllocator<std::pair<const uint256, CBlockIndex>, 512, 8>>;

/** Select one backend before loading any indexes. The reference backend retains
 * the pre-PR #10 map type for differential tests. Both production startup modes
 * use the standard map with the existing pool allocator and a local non-throwing
 * version of the same hash. No custom object storage or disk trust is involved.
 */
class BlockMap {
public:
    using Legacy = std::unordered_map<uint256, CBlockIndex, BlockHasher>;
    using value_type = Legacy::value_type;
private:
    // Destruction order keeps the pool alive until all map nodes are freed.
    std::unique_ptr<PoolResource<512, 8>> m_fast_resource;
    std::variant<Legacy, std::unique_ptr<FastBlockMap>> m_backend;

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
        using FastIterator = std::conditional_t<Constant, FastBlockMap::const_iterator, FastBlockMap::iterator>;
        Owner* m_owner{nullptr};
        std::variant<LegacyIterator, FastIterator> m_iterator;
        template <class Map, class It>
        Iterator(Owner* owner, It it, Map*)
            : m_owner{owner}, m_iterator{std::in_place_index<std::is_same_v<std::decay_t<Map>, Legacy> ? 0 : 1>, it} {}
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

    explicit BlockMap(bool fast = false)
    {
        if (fast) {
            m_fast_resource = std::make_unique<PoolResource<512, 8>>();
            m_backend.emplace<1>(std::make_unique<FastBlockMap>(
                0, FastBlockHasher{}, std::equal_to<uint256>{}, FastBlockMap::allocator_type{m_fast_resource.get()}));
        }
    }
    BlockMap(const BlockMap&) = delete;
    BlockMap& operator=(const BlockMap&) = delete;
    bool IsFast() const { return m_backend.index() == 1; }
    size_t size() const { return Visit([](const auto& map) { return map.size(); }); }
    bool empty() const { return size() == 0; }
    size_t max_size() const { return Visit([](const auto& map) { return map.max_size(); }); }
    size_t bucket_count() const { return Visit([](const auto& map) { return map.bucket_count(); }); }
    float max_load_factor() const { return Visit([](const auto& map) { return map.max_load_factor(); }); }
    iterator begin() { return Visit([&](auto& map) { return iterator{this, map.begin(), &map}; }); }
    iterator end() { return Visit([&](auto& map) { return iterator{this, map.end(), &map}; }); }
    const_iterator begin() const { return Visit([&](const auto& map) { return const_iterator{this, map.begin(), &map}; }); }
    const_iterator end() const { return Visit([&](const auto& map) { return const_iterator{this, map.end(), &map}; }); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator find(const uint256& key) { return Visit([&](auto& map) { return iterator{this, map.find(key), &map}; }); }
    const_iterator find(const uint256& key) const { return Visit([&](const auto& map) { return const_iterator{this, map.find(key), &map}; }); }
    bool contains(const uint256& key) const { return Visit([&](const auto& map) { return map.contains(key); }); }
    size_t count(const uint256& key) const { return contains(key) ? 1 : 0; }
    CBlockIndex& operator[](const uint256& key) { return try_emplace(key).first->second; }
    void reserve(size_t count) { Visit([&](auto& map) { map.reserve(count); }); }
    template <class... Args> std::pair<iterator, bool> try_emplace(const uint256& key, Args&&... args)
    {
        return Visit([&](auto& map) {
            auto [it, inserted]{map.try_emplace(key, std::forward<Args>(args)...)};
            return std::pair<iterator, bool>{iterator{this, it, &map}, inserted};
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
        if (IsFast()) {
            auto& map{*std::get<1>(m_backend)};
            return iterator{this, map.erase(std::get<1>(it.m_iterator)), &map};
        }
        auto& map{std::get<0>(m_backend)};
        return iterator{this, map.erase(std::get<0>(it.m_iterator)), &map};
    }
};
} // namespace node
#endif // BITCOIN_NODE_BLOCKMAP_H
