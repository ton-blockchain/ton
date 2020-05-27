#include "PeerState.h"
namespace ton {
void PeerState::notify_node() {
  if (node.empty()) {
    return;
  }
  td::actor::send_signals_later(node, td::actor::ActorSignals::wakeup());
}

void PeerState::notify_peer() {
  if (peer.empty()) {
    return;
  }
  td::actor::send_signals_later(peer, td::actor::ActorSignals::wakeup());
}
}  // namespace ton
