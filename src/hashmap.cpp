// TinyKV — hand-rolled hash table implementation.
#include "hashmap.hpp"

namespace tinykv {

HashMap::HashMap(std::size_t initial_capacity) {
    if (initial_capacity < 8) {
        initial_capacity = 8;
    }
    buckets_.assign(initial_capacity, nullptr);
}

HashMap::~HashMap() {
    for (Node* head : buckets_) {
        while (head != nullptr) {
            Node* nxt = head->next;
            delete head;
            head = nxt;
        }
    }
}

bool HashMap::insert(const std::string& key, const std::string& value) {
    if (load_factor() > kMaxLoadFactor) {
        rehash(buckets_.size() * 2);
    }
    std::size_t idx = bucket_index(key);
    for (Node* n = buckets_[idx]; n != nullptr; n = n->next) {
        if (n->key == key) {
            n->value = value;
            return false;  // overwritten
        }
    }
    buckets_[idx] = new Node(key, value, buckets_[idx]);
    ++size_;
    return true;  // newly added
}

bool HashMap::erase(const std::string& key) {
    std::size_t idx = bucket_index(key);
    Node* prev = nullptr;
    for (Node* n = buckets_[idx]; n != nullptr; n = n->next) {
        if (n->key == key) {
            if (prev == nullptr) {
                buckets_[idx] = n->next;
            } else {
                prev->next = n->next;
            }
            delete n;
            --size_;
            return true;
        }
        prev = n;
    }
    return false;
}

bool HashMap::find(const std::string& key, std::string& value_out) const {
    std::size_t idx = bucket_index(key);
    for (Node* n = buckets_[idx]; n != nullptr; n = n->next) {
        if (n->key == key) {
            value_out = n->value;
            return true;
        }
    }
    return false;
}

void HashMap::for_each(
    const std::function<void(const std::string&, const std::string&)>& fn) const {
    for (Node* head : buckets_) {
        for (Node* n = head; n != nullptr; n = n->next) {
            fn(n->key, n->value);
        }
    }
}

void HashMap::rehash(std::size_t new_capacity) {
    std::vector<Node*> grown(new_capacity, nullptr);
    for (Node* head : buckets_) {
        Node* n = head;
        while (n != nullptr) {
            Node* nxt = n->next;
            std::size_t idx = static_cast<std::size_t>(
                fnv1a(n->key.data(), n->key.size()) % new_capacity);
            n->next = grown[idx];
            grown[idx] = n;
            n = nxt;
        }
    }
    buckets_.swap(grown);
}

}  // namespace tinykv
