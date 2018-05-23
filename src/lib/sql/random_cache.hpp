#pragma once

#include <list>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "abstract_cache.hpp"
#include "utils/assert.hpp"

namespace opossum {

// Generic cache implementation using a random eviction policy.
// Note: This implementation is not thread-safe.
template <typename Key, typename Value>
class RandomCache : public AbstractCache<Key, Value> {
 public:
  typedef typename std::pair<Key, Value> KeyValuePair;

  explicit RandomCache(size_t capacity) : AbstractCache<Key, Value>(capacity), _gen(_rd()), _rand(0, capacity - 1) {
    _list.reserve(capacity);
  }

  // Sets the value to be cached at the given key.
  std::optional<Key> set(const Key& key, const Value& value, double cost = 1.0, double size = 1.0) {
    // Override old element at that key, if it exists.
    auto it = _map.find(key);
    if (it != _map.end()) {
      // it->second->second = value;
      _list[it->second].second = value;
      return {};
    }

    // If capacity is exceeded, pick a random element and replace it.
    if (_list.size() >= this->_capacity) {
      size_t index = _rand(_gen);
      auto evicted = std::make_optional(_list[index].first);
      _map.erase(_list[index].first);

      _list[index] = KeyValuePair(key, value);
      // _map[key] = _list.begin() + index;
      _map[key] = index;
      return evicted;
    }

    // Otherwise simply add to the end of the vector.
    _list.push_back(KeyValuePair(key, value));
    // _map[key] = _list.begin() + (_list.size() - 1);
    _map[key] = _list.size() - 1;

    return {};
  }

  // Retrieves the value cached at the key.
  Value& get(const Key& key) {
    auto it = _map.find(key);
    // if (it == _map.end()) {
    //   Fail("Wusa");
    // }
    return _list[it->second].second;
    // return it->second->second;
  }

  bool has(const Key& key) const { return _map.find(key) != _map.end(); }

  size_t size() const { return _map.size(); }

  void clear() {
    _list.clear();
    _map.clear();
  }

  void clear_and_resize(size_t capacity) {
    clear();
    this->_capacity = capacity;
    _list.shrink_to_fit();
    _list.reserve(capacity);
    _rand = std::uniform_int_distribution<>(0, capacity - 1);
  }

  void resize(size_t capacity) {
    if (_list.size() > capacity) {
      for (size_t difference = 0; difference < _list.size() - capacity; ++difference) {
        _map.erase(_list[difference].first);
        _list.erase(_list.cbegin() + difference);
      }
    }

    this->_capacity = capacity;
    _rand = std::uniform_int_distribution<>(0, capacity - 1);
  }

  std::vector<Key> dump_cache() {
    std::vector<Key> cache;
    for (auto& kv : _list) {
      cache.push_back(kv.first);
    }
    return cache;
  }

 protected:
  // List to hold all elements.
  std::vector<KeyValuePair> _list;

  // Map to point towards element in the list.
  std::unordered_map<Key, size_t> _map;

  // Random number generation to determine which item to evict.
  std::random_device _rd;
  std::mt19937 _gen;
  std::uniform_int_distribution<> _rand;
};

}  // namespace opossum
