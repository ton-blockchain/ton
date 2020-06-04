#pragma once

#include "LoadSpeed.h"
#include "PartsHelper.h"
#include "PeerActor.h"
#include "Torrent.h"

#include "td/utils/Random.h"

#include <map>

namespace ton {
class NodeActor : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() {
    }
    virtual td::actor::ActorOwn<PeerActor> create_peer(PeerId self_id, PeerId peer_id,
                                                       td::SharedState<PeerState> state) = 0;
    virtual void get_peers(td::Promise<std::vector<PeerId>> peers) = 0;
    virtual void register_self(td::actor::ActorId<ton::NodeActor> self) = 0;

    //TODO: proper callbacks
    virtual void on_completed() = 0;
    virtual void on_closed(ton::Torrent torrent) = 0;
  };

  NodeActor(PeerId self_id, ton::Torrent torrent, td::unique_ptr<Callback> callback, bool should_download = true);
  void start_peer(PeerId peer_id, td::Promise<td::actor::ActorId<PeerActor>> promise);

  ton::Torrent *with_torrent() {
    return &torrent_;
  }
  std::string get_stats_str();

  void set_file_priority(size_t i, td::uint8 priority);
  void set_should_download(bool should_download);

 private:
  PeerId self_id_;
  ton::Torrent torrent_;
  std::vector<td::uint8> file_priority_;
  td::unique_ptr<Callback> callback_;
  bool should_download_{false};

  class Notifier : public td::actor::Actor {
   public:
    Notifier(td::actor::ActorId<NodeActor> node, PeerId peer_id) : node_(std::move(node)), peer_id_(peer_id) {
    }

    void wake_up() override {
      send_closure(node_, &NodeActor::on_signal_from_peer, peer_id_);
    }

   private:
    td::actor::ActorId<NodeActor> node_;
    PeerId peer_id_;
  };

  struct Peer {
    td::actor::ActorOwn<PeerActor> actor;
    td::actor::ActorOwn<Notifier> notifier;
    td::SharedState<PeerState> state;
    PartsHelper::PeerToken peer_token;
  };

  std::map<PeerId, Peer> peers_;

  struct QueryId {
    PeerId peer;
    PartId part;

    auto key() const {
      return std::tie(peer, part);
    }
    bool operator<(const QueryId &other) const {
      return key() < other.key();
    }
  };

  struct PartsSet {
    struct Info {
      td::optional<PeerId> query_to_peer;
      bool ready{false};
    };
    size_t total_queries{0};
    std::vector<Info> parts;
  };

  PartsSet parts_;
  PartsHelper parts_helper_;
  std::vector<PartId> ready_parts_;
  LoadSpeed download_;

  td::Timestamp next_get_peers_at_;
  bool has_get_peers_{false};
  static constexpr double GET_PEER_RETRY_TIMEOUT = 5;
  static constexpr double GET_PEER_EACH = 5;

  bool is_completed_{false};

  td::Timestamp will_upload_at_;

  void on_signal_from_peer(PeerId peer_id);

  void start_up() override;

  void loop() override;

  void tear_down() override;

  void loop_start_stop_peers();

  static constexpr size_t MAX_TOTAL_QUERIES = 20;
  static constexpr size_t MAX_PEER_TOTAL_QUERIES = 5;
  void loop_queries();
  bool try_send_query();
  bool try_send_part(PartId part_id);
  void loop_get_peers();
  void got_peers(td::Result<std::vector<PeerId>> r_peers);
  void loop_peer(const PeerId &peer_id, Peer &peer);
  void on_part_ready(PartId part_id);

  void loop_will_upload();
};
}  // namespace ton
