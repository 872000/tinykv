// TinyKV — hand-rolled hash table.
//
// Separate chaining with dynamic resizing. Deliberately does NOT use
// std::unordered_map: the bucket array, hash function (FNV-1a) and the
// rehash logic are implemented from scratch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tinykv {

// FNV-1a 64-bit hash.
inline std::uint64_t fnv1a(const char* data, std::size_t len) {
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

class HashMap {
public:
    struct Node {
        std::string key;
        std::string value;
        Node* next;
        Node(std::string k, std::string v, Node* n)
            : key(std::move(k)), value(std::move(v)), next(n) {}
    };

    explicit HashMap(std::size_t initial_capacity = 16);
    ~HashMap();

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    // Insert or overwrite. Returns true when the key is newly added.
    bool insert(const std::string& key, const std::string& value);
    // Returns true when a key was actually removed.
    bool erase(const std::string& key);
    bool find(const std::string& key, std::string& value_out) const;

    std::size_t size() const { return size_; }
    std::size_t bucket_count() const { return buckets_.size(); }
    double load_factor() const {
        return static_cast<double>(size_) / static_cast<double>(buckets_.size());
    }

    void for_each(const std::function<void(const std::string& key,
                                           const std::string& value)>& fn) const;

private:
    std::size_t bucket_index(const std::string& key) const {
        return static_cast<std::size_t>(fnv1a(key.data(), key.size()) %
                                        buckets_.size());
    }
    void rehash(std::size_t new_capacity);

    std::vector<Node*> buckets_;
    std::size_t size_ = 0;
    static constexpr double kMaxLoadFactor = 0.75;
};

}  // namespace tinykv
