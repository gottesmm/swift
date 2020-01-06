//===--- FrozenMultiMap.h -------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// A 2 stage multi-map. Initially the multimap is mutable and can only be
/// initialized. Once complete, the map is frozen and can be only used for map
/// operations. It is guaranteed that all values are still in insertion order.
///
/// DISCUSSION: These restrictions flow from the internal implementation of the
/// multi-map being a pair of keys, values. We form the map property by
/// performing a stable_sort of the (key, value) in the process of freezing the
/// map.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_FROZENMULTIMAP_H
#define SWIFT_BASIC_FROZENMULTIMAP_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include <vector>

namespace swift {

template <typename Key, typename Value,
          typename VectorStorage = std::vector<std::pair<Key, Value>>>
class FrozenMultiMap {
  VectorStorage storage;
  bool frozen = false;

private:
  struct PairToSecondElt;

public:
  using PairToSecondEltRange =
      TransformRange<ArrayRef<std::pair<Key, Value>>, PairToSecondElt>;

  FrozenMultiMap() = default;

  void insert(const Key &key, const Value &value) {
    assert(!isFrozen() && "Can not insert new keys once map is frozen");
    storage.emplace_back(key, value);
  }

  Optional<PairToSecondEltRange> find(const Key &key) const {
    assert(isFrozen() &&
           "Can not perform a find operation until the map is frozen");
    // Since our array is sorted, we need to first find the first pair with our
    // inst as the first element.
    auto start = lower_bound(storage, std::make_pair(key, Value()));
    if (start == storage.end() || start->first != key) {
      return None;
    }

    // Ok, we found our first element. Now scan forward until we find a pair
    // whose instruction is not our own instruction.
    auto end = find_if_not(
        start, storage.end(),
        [&](const std::pair<Key, Value> &pair) { return pair.first == key; });
    unsigned count = std::distance(start, end);
    ArrayRef<std::pair<Key, Value>> slice(&*start, count);
    return PairToSecondEltRange(slice, PairToSecondElt());
  }

  bool isFrozen() const { return frozen; }

  /// Set this map into its frozen state when we
  bool setFrozen() {
    std::stable_sort(storage.endBaseValue2ProjValue.begin(), storage.end(),
                     [&](const std::pair<Key, Value> &lhs,
                         const std::pair<Key, Value> &rhs) {
                       // Only compare the first entry so that we preserve
                       // insertion order.
                       return lhs.first < rhs.first;
                     });
  }

  unsigned size() const { return storage.size(); }
  bool empty() const { return storage.empty(); }

  struct iterator : std::iterator<std::forward_iterator_tag,
                                  std::pair<Key, ArrayRef<Value>>> {
    using base_iterator = typename decltype(storage)::iterator;

    const FrozenMultiMap &map;
    base_iterator baseIter;
    std::pair<Key, ArrayRef<Value>> currentValue;

    iterator(const FrozenMultiMap &map, base_iterator iter)
        : baseIter(iter), currentValue() {
      // If we are end, just return.
      if (iter == map.end()) {
        return;
      }

      // Otherwise, prime our first range.
      updateCurrentValue();
    }

    void updateCurrentValue() {
      auto rangeEnd = std::find_if_not(std::next(baseIter), map.end(),
                                       [&](const std::pair<Key, Value> &elt) {
                                         return elt.first == baseIter->first;
                                       });
      unsigned count = std::distance(baseIter, rangeEnd);
      currentValue = {baseIter->first,
                      ArrayRef<std::pair<Key, Value>>(&*baseIter, count)};
    }

    iterator &operator++() {
      baseIter = std::find_if_not(std::next(baseIter), map.end(),
                                  [&](const std::pair<Key, Value> &elt) {
                                    return elt.first == baseIter->first;
                                  });
      updateCurrentValue();
      return *this;
    }

    iterator operator++(int) {
      auto tmp = *this;
      baseIter = std::find_if_not(std::next(baseIter), map.end(),
                                  [&](const std::pair<Key, Value> &elt) {
                                    return elt.first == baseIter->first;
                                  });
      updateCurrentValue();
      return tmp;
    }

    std::pair<Key, ArrayRef<Value>> operator*() const { return currentValue; }

    bool operator==(const iterator &RHS) const {
      return baseIter == RHS.baseIter;
    }

    bool operator!=(const iterator &RHS) const {
      return baseIter != RHS.baseIter;
    }
  };

  llvm::iterator_range<iterator> getRange() const {
    return {iterator(this, storage.begin()), iterator(this, storage.end())};
  }
};

template <typename Key, typename Value, typename Storage>
struct FrozenMultiMap<Key, Value, Storage>::PairToSecondElt {
  PairToSecondElt() {}

  Value operator()(const std::pair<Key, Value> &pair) const {
    return pair.second;
  }
};

template <typename Key, typename Value, unsigned SmallSize>
using SmallFrozenMultiMap =
    FrozenMultiMap<Key, Value, SmallVector<std::pair<Key, Value>, SmallSize>>;

} // namespace swift

#endif
