#include "adnl-tunnel.h"
#include "adnl-peer-table.h"

namespace ton {

namespace adnl {

void AdnlInboundTunnelEndpoint::receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) {
  receive_packet_cont(src, src_addr, std::move(datagram), 0);
}

void AdnlInboundTunnelEndpoint::receive_packet_cont(AdnlNodeIdShort src, td::IPAddress src_addr,
                                                    td::BufferSlice datagram, size_t idx) {
  if (datagram.size() <= 32) {
    VLOG(ADNL_INFO) << "dropping too short datagram";
    return;
  }
  if (datagram.as_slice().truncate(32) != decrypt_via_[idx].as_slice()) {
    VLOG(ADNL_INFO) << "invalid tunnel midpoint";
    return;
  }
  datagram.confirm_read(32);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), src, src_addr, idx](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      VLOG(ADNL_INFO) << "dropping tunnel packet: failed to decrypt: " << R.move_as_error();
      return;
    } else {
      td::actor::send_closure(SelfId, &AdnlInboundTunnelEndpoint::decrypted_packet, src, src_addr, R.move_as_ok(), idx);
    }
  });
  td::actor::send_closure(keyring_, &keyring::Keyring::decrypt_message, decrypt_via_[idx], std::move(datagram),
                          std::move(P));
}

void AdnlInboundTunnelEndpoint::decrypted_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice data,
                                                 size_t idx) {
  if (idx == decrypt_via_.size() - 1) {
    td::actor::send_closure(adnl_, &AdnlPeerTable::receive_packet, src_addr, std::move(data));
    return;
  }
  auto F = fetch_tl_object<ton_api::adnl_tunnelPacketContents>(std::move(data), true);
  if (F.is_error()) {
    VLOG(ADNL_INFO) << "dropping tunnel packet: failed to fetch: " << F.move_as_error();
    return;
  }
  auto packet = F.move_as_ok();

  td::IPAddress addr;
  if (packet->flags_ & 1) {
    addr.init_host_port(td::IPAddress::ipv4_to_str(packet->from_ip_), packet->from_port_).ignore();
  }

  if (packet->flags_ & 2) {
    receive_packet_cont(src, addr, std::move(packet->message_), idx + 1);
  }
}

void AdnlInboundTunnelMidpoint::start_up() {
  encrypt_key_hash_ = encrypt_via_.compute_short_id();
  auto R = encrypt_via_.create_encryptor();
  if (R.is_error()) {
    return;
  }
  encryptor_ = R.move_as_ok();
}

void AdnlInboundTunnelMidpoint::receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) {
  if (!encryptor_) {
    return;
  }
  auto obj = create_tl_object<ton_api::adnl_tunnelPacketContents>();
  obj->flags_ = 2;
  obj->message_ = std::move(datagram);
  if (src_addr.is_valid() && src_addr.is_ipv4()) {
    obj->flags_ |= 1;
    obj->from_ip_ = src_addr.get_ipv4();
    obj->from_port_ = src_addr.get_port();
  }
  auto packet = serialize_tl_object(std::move(obj), true);
  auto dataR = encryptor_->encrypt(packet.as_slice());
  if (dataR.is_error()) {
    return;
  }
  auto data = dataR.move_as_ok();
  td::BufferSlice enc{data.size() + 32};
  auto S = enc.as_slice();
  S.copy_from(encrypt_key_hash_.as_slice());
  S.remove_prefix(32);
  S.copy_from(data.as_slice());

  td::actor::send_closure(adnl_, &Adnl::send_message_ex, proxy_as_, proxy_to_, std::move(enc),
                          Adnl::SendFlags::direct_only);
}

}  // namespace adnl
}  // namespace ton
