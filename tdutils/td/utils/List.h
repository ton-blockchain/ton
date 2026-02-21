/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "td/utils/common.h"

namespace td {

struct DefaultListTag {};

template <class Tag = DefaultListTag>
struct TaggedListNode {
  TaggedListNode *next;
  TaggedListNode *prev;
  TaggedListNode() {
    clear();
  }

  ~TaggedListNode() {
    remove();
  }

  TaggedListNode(const TaggedListNode &) = delete;
  TaggedListNode &operator=(const TaggedListNode &) = delete;

  TaggedListNode(TaggedListNode &&other) {
    if (other.empty()) {
      clear();
    } else {
      init_from(std::move(other));
    }
  }

  TaggedListNode &operator=(TaggedListNode &&other) {
    if (this == &other) {
      return *this;
    }

    this->remove();

    if (!other.empty()) {
      init_from(std::move(other));
    }

    return *this;
  }

  void connect(TaggedListNode *to) {
    CHECK(to != nullptr);
    next = to;
    to->prev = this;
  }

  void remove() {
    prev->connect(next);
    clear();
  }

  void put(TaggedListNode *other) {
    DCHECK(other->empty());
    put_unsafe(other);
  }

  void put_back(TaggedListNode *other) {
    DCHECK(other->empty());
    prev->connect(other);
    other->connect(this);
  }

  TaggedListNode *get() {
    TaggedListNode *result = prev;
    if (result == this) {
      return nullptr;
    }
    result->prev->connect(this);
    result->clear();
    return result;
  }

  bool empty() const {
    return next == this;
  }

  TaggedListNode *begin() {
    return next;
  }
  TaggedListNode *end() {
    return this;
  }
  TaggedListNode *get_next() {
    return next;
  }
  TaggedListNode *get_prev() {
    return prev;
  }

 protected:
  void clear() {
    next = this;
    prev = this;
  }

  void init_from(TaggedListNode &&other) {
    TaggedListNode *head = other.prev;
    other.remove();
    head->put_unsafe(this);
  }

  void put_unsafe(TaggedListNode *other) {
    other->connect(next);
    this->connect(other);
  }
};

using ListNode = TaggedListNode<>;

}  // namespace td
