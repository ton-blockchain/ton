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
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/optional.h"

#include "td/actor/actor.h"

#include <map>
#include <atomic>

namespace ton {
using PeerId = td::uint64;
using PartId = td::uint32;

// Concurrent buffer for messages with one writer and one reader
// Reader reads all existing messages at once
// TODO: Use some better algorithm here, or maybe a concurrent queue
template <typename T>
class MessageBuffer {
 public:
  MessageBuffer() = default;
  MessageBuffer(const MessageBuffer<T>&) = delete;
  MessageBuffer& operator=(const MessageBuffer<T>&) = delete;
  ~MessageBuffer() {
    delete ptr_.load();
  }

  void add_element(T x) {
    std::vector<T>* vec = ptr_.exchange(nullptr);
    if (vec == nullptr) {
      vec = new std::vector<T>();
    }
    vec->push_back(std::move(x));
    CHECK(ptr_.exchange(vec) == nullptr);
  }

  void add_elements(std::vector<T> elements) {
    if (elements.empty()) {
      return;
    }
    std::vector<T>* vec = ptr_.exchange(nullptr);
    if (vec == nullptr) {
      vec = new std::vector<T>(std::move(elements));
    } else {
      for (auto& x : elements) {
        vec->push_back(std::move(x));
      }
    }
    CHECK(ptr_.exchange(vec) == nullptr);
  }

  std::vector<T> read() {
    std::vector<T>* vec = ptr_.exchange(nullptr);
    std::vector<T> result;
    if (vec != nullptr) {
      result = std::move(*vec);
      delete vec;
    }
    return result;
  }
 private:
  std::atomic<std::vector<T>*> ptr_{nullptr};
};

struct PeerState {
  explicit PeerState(td::actor::ActorId<> node) : node(std::move(node)) {
  }

  struct State {
    bool will_upload;
    bool want_download;
    auto key() const {
      return std::tie(will_upload, want_download);
    }
    bool operator==(const State &other) const {
      return key() == other.key();
    }
  };
  // Thread-safe fields
  std::atomic<State> node_state_{State{false, false}};
  std::atomic_bool peer_state_ready_{false};
  std::atomic<State> peer_state_{State{false, false}};
  std::atomic_bool peer_online_{false};

  struct Part {
    td::BufferSlice proof;
    td::BufferSlice data;
  };

  std::set<PartId> node_queries_active_; // Node only
  MessageBuffer<PartId> node_queries_; // Node -> Peer
  MessageBuffer<std::pair<PartId, td::Result<Part>>> node_queries_results_; // Peer -> Node

  std::set<PartId> peer_queries_active_; // Peer only
  MessageBuffer<PartId> peer_queries_; // Peer -> Node
  MessageBuffer<std::pair<PartId, td::Result<Part>>> peer_queries_results_; // Node -> Peer

  // Peer -> Node
  MessageBuffer<PartId> peer_ready_parts_;
  // Node -> Peer
  MessageBuffer<PartId> node_ready_parts_;

  // Node -> Peer
  std::atomic_bool torrent_info_ready_{false};
  std::shared_ptr<td::BufferSlice> torrent_info_str_;
  std::function<void(td::BufferSlice)> torrent_info_response_callback_;

  const td::actor::ActorId<> node;
  std::atomic_bool peer_ready_{false};
  td::actor::ActorId<> peer;

  void notify_node();
  void notify_peer();
};
}  // namespace ton
