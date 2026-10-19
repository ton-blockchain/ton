/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#pragma once

#include <cstddef>

#include "td/utils/Time.h"
#include "td/utils/logging.h"

namespace ton::validator::detail {

// An intrusive FIFO whose entries all have the same lifetime. Appending in
// expiry order makes removing expired entries proportional to the number that
// actually expire, rather than to the number stored.
//
// Entry must expose:
//   const td::Timestamp delete_at;
//   Entry *older;
//   Entry *newer;
template <class Entry>
class ExpiryOrderedList {
 public:
  ExpiryOrderedList() = default;
  ExpiryOrderedList(const ExpiryOrderedList &) = delete;
  ExpiryOrderedList &operator=(const ExpiryOrderedList &) = delete;

  Entry *oldest() const {
    return oldest_;
  }

  Entry *newest() const {
    return newest_;
  }

  bool empty() const {
    return oldest_ == nullptr;
  }

  bool contains(const Entry *entry) const {
    return entry != nullptr && (entry == oldest_ || entry->older != nullptr || entry->newer != nullptr);
  }

  void append(Entry *entry) {
    CHECK(entry != nullptr);
    CHECK(entry != oldest_ && entry != newest_);
    CHECK(entry->older == nullptr && entry->newer == nullptr);
    CHECK(newest_ == nullptr || newest_->delete_at <= entry->delete_at);

    entry->older = newest_;
    if (newest_ != nullptr) {
      newest_->newer = entry;
    } else {
      oldest_ = entry;
    }
    newest_ = entry;
  }

  void unlink(Entry *entry) {
    CHECK(entry != nullptr);
    CHECK(entry == oldest_ || entry->older != nullptr);
    CHECK(entry == newest_ || entry->newer != nullptr);

    if (entry->older != nullptr) {
      entry->older->newer = entry->newer;
    } else {
      oldest_ = entry->newer;
    }
    if (entry->newer != nullptr) {
      entry->newer->older = entry->older;
    } else {
      newest_ = entry->older;
    }
    entry->older = nullptr;
    entry->newer = nullptr;
  }

  template <class Erase>
  size_t pop_expired(td::Timestamp now, Erase &&erase) {
    size_t removed = 0;
    while (oldest_ != nullptr && oldest_->delete_at.is_in_past(now)) {
      Entry *entry = oldest_;
      CHECK(erase(entry));
      CHECK(oldest_ != entry);  // Erase must unlink the entry.
      ++removed;
    }
    return removed;
  }

  void relax_alarm(td::Timestamp &alarm) const {
    if (oldest_ != nullptr) {
      alarm.relax(oldest_->delete_at);
    }
  }

 private:
  Entry *oldest_{nullptr};
  Entry *newest_{nullptr};
};

}  // namespace ton::validator::detail
