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
#include "td/net/UdpServer.h"
#include "td/net/FdListener.h"
#include "td/net/TcpListener.h"

#include "td/net/tunnel/libtunnel.h"

#include "td/utils/BufferedFd.h"

#include <map>

namespace td {
namespace {
int VERBOSITY_NAME(udp_server) = VERBOSITY_NAME(DEBUG) + 10;
}
namespace detail {

class UdpServerTunnelImpl : public UdpServer {
 public:
  void start_up() override;
  void alarm() override;

  void send(td::UdpMessage &&message) override;
  static td::actor::ActorOwn<UdpServerTunnelImpl> create(td::Slice name, int32 port, std::unique_ptr<Callback> callback,
                                                         td::Promise<td::IPAddress> on_ready);

  UdpServerTunnelImpl(int32 port, std::unique_ptr<Callback> callback, td::Promise<td::IPAddress> on_ready);

private:
  td::Promise<td::IPAddress> on_ready_;
  char out_buf_[(16+2+1500)*300];
  size_t out_buf_offset_ = 0;
  size_t out_buf_msg_num_ = 0;
  size_t tunnel_index_;
  double last_batch_at_ = Time::now();

  int32 port_;
  std::unique_ptr<Callback> callback_;

  static void on_recv_batch(void *next, char *data, size_t num);

};

void UdpServerTunnelImpl::send(td::UdpMessage &&message) {
  auto sock = message.address.get_sockaddr();
  auto sz = message.data.size();

  // ip+port
  memcpy(out_buf_ + out_buf_offset_, sock, sizeof(sockaddr));
  out_buf_offset_ += sizeof(sockaddr);

  // data len (2 bytes)
  out_buf_[out_buf_offset_] = static_cast<char>(sz >> 8);
  out_buf_[out_buf_offset_ + 1] = static_cast<char>(sz & 0xff);

  memcpy(out_buf_ + out_buf_offset_ + 2, message.data.data(), sz);
  out_buf_offset_ += 2 + sz;
  out_buf_msg_num_++;


  if (out_buf_msg_num_ >= 100) {
    td::Timer timer;
    WriteTunnel(tunnel_index_, out_buf_, out_buf_msg_num_);
    LOG(INFO) << "Sending messages " << out_buf_msg_num_ << " | " << timer.elapsed();

    out_buf_offset_ = 0;
    out_buf_msg_num_ = 0;
    last_batch_at_ = Time::now();
  }
  // LOG(INFO) << "TUN message to: ";
}

void UdpServerTunnelImpl::alarm() {
  auto now = Time::now();
  if (out_buf_msg_num_ > 0 && now-last_batch_at_ > 0.02) {
    td::Timer timer;
    WriteTunnel(tunnel_index_, out_buf_, out_buf_msg_num_);
    LOG(ERROR) << "Sending messages from alarm " << out_buf_msg_num_ << " | " << timer.elapsed();

    out_buf_offset_ = 0;
    out_buf_msg_num_ = 0;
    last_batch_at_ = now;
  }

  alarm_timestamp() = td::Timestamp::in(0.05);
}

void UdpServerTunnelImpl::start_up() {
  auto cfg = Slice("{\n\t\t\"TunnelServerKey\": \"2Kg9YuGSbpPA8+2UlMyef47UOzFt7fFjz9QREGUhP0U=\",\n\t\t\"TunnelThreads\": 10,\n\t\t\"PaymentsEnabled\": false,\n\t\t\"Payments\": {\n\t\t\t\"PaymentsServerKey\": \"8BfanhwJYWRlASVw6mXeuUxoMyB73CDfHYLQ8mF5FxE=\",\n\t\t\t\"WalletPrivateKey\": \"8BfanhwJYWRlASVw6mXeuUxoMyB73CDfHYLQ8mF5FxA=\",\n\t\t\t\"PaymentsListenAddr\": \"0.0.0.0:13131\",\n\t\t\t\"DBPath\": \"./payments-db/\",\n\t\t\t\"SecureProofPolicy\": false,\n\t\t\t\"ChannelConfig\": {\n\t\t\t\t\"VirtualChannelProxyFee\": \"0.01\",\n\t\t\t\t\"QuarantineDurationSec\": 600,\n\t\t\t\t\"MisbehaviorFine\": \"0.15\",\n\t\t\t\t\"ConditionalCloseDurationSec\": 180\n\t\t\t}\n\t\t},\n\t\t\"OutGateway\": {\n\t\t\t\"Key\": \"cKrWi/IgAKvd3Ro92ap2IfKABX3C4rQI59P+v1g+vOg=\",\n\t\t\t\"Payment\": null},\n\t\t\"RouteOut\": [],\n\t\t\"RouteIn\": []\n\t}");
  auto netCfg = Slice("{\n\t\"liteservers\": [\n\t\t{\n\t\t\t\"ip\": 822907680,\n\t\t\t\"port\": 27842,\n\t\t\t\"provided\":\"Beavis\",\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"sU7QavX2F964iI9oToP9gffQpCQIoOLppeqL/pdPvpM=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": -1468571697,\n\t\t\t\"port\": 27787,\n\t\t\t\"provided\":\"Beavis\",\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"Y/QVf6G5VDiKTZOKitbFVm067WsuocTN8Vg036A4zGk=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": -1468575011,\n\t\t\t\"port\": 51088,\n\t\t\t\"provided\":\"Beavis\",\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"Sy5ghr3EahQd/1rDayzZXt5+inlfF+7kLfkZDJcU/ek=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1844203537,\n\t\t\t\"port\": 37537,\n\t\t\t\"provided\":\"Neo\",\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"K1F7zEe0ETf+SwkefLS56hJE8x42sjCVsBJJuaY7nEA=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1844203589,\n\t\t\t\"port\": 34411,\n\t\t\t\"provided\":\"Neo\",\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"pOpRRpIxDuMRm1qFUPpvVjD62vo8azkO0npw4FPcW/I=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1047529523,\n\t\t\t\"port\": 37649,\n\t\t\t\"provided\":\"Neo\",\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"pRf2sAa7d+Chl8gDclWOMtthtxjKnLYeAIzk869mMvA=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1592601963,\n\t\t\t\"port\": 13833,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"QpVqQiv1u3nCHuBR3cg3fT6NqaFLlnLGbEgtBRukDpU=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1162057690,\n\t\t\t\"port\": 35939,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"97y55AkdzXWyyVuOAn+WX6p66XTNs2hEGG0jFUOkCIo=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": -1304477830,\n\t\t\t\"port\": 20700,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"dGLlRRai3K9FGkI0dhABmFHMv+92QEVrvmTrFf5fbqA=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1959453117,\n\t\t\t\"port\": 20700,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"24RL7iVI20qcG+j//URfd/XFeEG9qtezW2wqaYQgVKw=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": -809760973,\n\t\t\t\"port\": 20700,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"vunMV7K35yPlTQPx/Fqk6s+4/h5lpcbP+ao0Cy3M2hw=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1097633201,\n\t\t\t\"port\": 17439,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"0MIADpLH4VQn+INHfm0FxGiuZZAA8JfTujRqQugkkA8=\"\n\t\t\t}\n\t\t},\n\t\t{\n\t\t\t\"ip\": 1091956407,\n\t\t\t\"port\": 16351,\n\t\t\t\"id\": {\n\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\"key\": \"Mf/JGvcWAvcrN3oheze8RF/ps6p7oL6ifrIzFmGQFQ8=\"\n\t\t\t}\n\t\t}\n\t],\n\t\"dht\": {\n\t\t\"a\": 3,\n\t\t\"k\": 3,\n\t\t\"static_nodes\": {\n\t\t\t\"nodes\": [\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"K2AWu8leN2RjYmhMpYAaGX/F6nGVk9oZw9c09RX3yyc=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": 1592601963,\n\t\t\t\t\t\t\t\t\"port\": 38723\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"21g16jxnqbb2ENAijrZFccHqLQcmmpkAI1HA46DaPvnVYvMkATFNEyHTy2R1T1jgU5M7CCLGJN+MxhwZfl/ZDA==\"\n\t\t\t\t},\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"fVIJzD9ATMilaPd847eFs6PtGSB67C+D9b4R+nf1+/s=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": 1097649206,\n\t\t\t\t\t\t\t\t\"port\": 29081\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"wH0HEVT6yAfZZAoD5bF6J3EZWdSFwBGl1ZpOfhxZ0Bp2u52tv8OzjeH8tlZ+geMLTG50Csn5nxSKP1tswTWwBg==\"\n\t\t\t\t},\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"gu+woR+x7PoRmaMqAP7oeOjK2V4U0NU8ofdacWZ34aY=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": 1162057690,\n\t\t\t\t\t\t\t\t\"port\": 41578\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"0PwDLXpN3IbRQuOTLkZBjkbT6+IkeUcvlhWrUY9us3IfSehmCfQjScR9mkVYsQ6cQHF+JeaFmqzV4GAiUcgjAg==\"\n\t\t\t\t},\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"WC4BO1eZ916FnLBSKmt07Pn5NP4D3/1wary1VjaCLaY=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": -1304477830,\n\t\t\t\t\t\t\t\t\"port\": 9670\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"cvpzkGeuEuKV+d92qIVkln9ngm8qeDnmYtK5rq8uSet0392hAZcIv2IniDzTw0rN42NaOHL9A4KEelwKu1N2Ag==\"\n\t\t\t\t},\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"nC8dcxV+EV2i0ARvub94IFJKKZUYACfY4xFj1NaG7Pw=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": 1959453117,\n\t\t\t\t\t\t\t\t\"port\": 63625\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"AHF6joNvQhyFFE0itV4OMA9n3Q8CEHVKapCLqazP7QJ4arsn4pdVkRYiGFEyQkngx+cm8izU4gB0JIaxF6PiBg==\"\n\t\t\t\t},\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"dqsRZLzTg/P7uxUlQpgl4VyTBNYBRMc4js3mnRiolBk=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": -809760973,\n\t\t\t\t\t\t\t\t\"port\": 40398\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"mJxLrAv5RamN5B9mDz6MhQwFjF92D3drJ5efOSZryDaazil0AR4bRHh4vxzZlYiPhi/X/NyG6WwNvKBz+1ntBw==\"\n\t\t\t\t},\n\t\t\t\t{\n\t\t\t\t\t\"@type\": \"dht.node\",\n\t\t\t\t\t\"id\": {\n\t\t\t\t\t\t\"@type\": \"pub.ed25519\",\n\t\t\t\t\t\t\"key\": \"fO6cFYRCRrD+yQzOJdHcNWpRFwu+qLhQnddLq0gGbTs=\"\n\t\t\t\t\t},\n\t\t\t\t\t\"addr_list\": {\n\t\t\t\t\t\t\"@type\": \"adnl.addressList\",\n\t\t\t\t\t\t\"addrs\": [\n\t\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\t\"@type\": \"adnl.address.udp\",\n\t\t\t\t\t\t\t\t\"ip\": 1097633201,\n\t\t\t\t\t\t\t\t\"port\": 7201\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t],\n\t\t\t\t\t\t\"version\": 0,\n\t\t\t\t\t\t\"reinit_date\": 0,\n\t\t\t\t\t\t\"priority\": 0,\n\t\t\t\t\t\t\"expire_at\": 0\n\t\t\t\t\t},\n\t\t\t\t\t\"version\": -1,\n\t\t\t\t\t\"signature\": \"o/rhtiUL3rvA08TKBcCn0DCiSjsNQdAv41aw7VVUig7ubaqJzYMv1cW3qMjxvsXn1BOugIheJm7voA1/brbtCg==\"\n\t\t\t\t}\n\t\t\t],\n\t\t\t\"@type\": \"dht.nodes\"\n\t\t},\n\t\t\"@type\": \"dht.config.global\"\n\t},\n\t\"@type\": \"config.global\",\n\t\"validator\": {\n\t\t\"zero_state\": {\n\t\t\t\"file_hash\": \"Z+IKwYS54DmmJmesw/nAD5DzWadnOCMzee+kdgSYDOg=\",\n\t\t\t\"seqno\": 0,\n\t\t\t\"root_hash\": \"gj+B8wb/AmlPk1z1AhVI484rhrUpgSr2oSFIh56VoSg=\",\n\t\t\t\"workchain\": -1,\n\t\t\t\"shard\": -9223372036854775808\n\t\t},\n\t\t\"@type\": \"validator.config.global\",\n\t\t\"init_block\": {\n\t\t\t\"workchain\": -1,\n\t\t\t\"shard\": -9223372036854775808,\n\t\t\t\"seqno\": 17908219,\n\t\t\t\"root_hash\": \"y6qWqhCnLgzWHjUFmXysaiOljuK5xVoCRMLzUwGInVM=\",\n\t\t\t\"file_hash\": \"Y/GziXxwuYte0AM4WT7tTWsCx+6rcfLpGmRaEQwhUKI=\"\n\t\t},\n\t\t\"hardforks\": [\n\t\t\t{\n\t\t\t\t\"file_hash\": \"jF3RTD+OyOoP+OI9oIjdV6M8EaOh9E+8+c3m5JkPYdg=\",\n\t\t\t\t\"seqno\": 5141579,\n\t\t\t\t\"root_hash\": \"6JSqIYIkW7y8IorxfbQBoXiuY3kXjcoYgQOxTJpjXXA=\",\n\t\t\t\t\"workchain\": -1,\n\t\t\t\t\"shard\": -9223372036854775808\n\t\t\t},\n\t\t\t{\n\t\t\t\t\"file_hash\": \"WrNoMrn5UIVPDV/ug/VPjYatvde8TPvz5v1VYHCLPh8=\",\n\t\t\t\t\"seqno\": 5172980,\n\t\t\t\t\"root_hash\": \"054VCNNtUEwYGoRe1zjH+9b1q21/MeM+3fOo76Vcjes=\",\n\t\t\t\t\"workchain\": -1,\n\t\t\t\t\"shard\": -9223372036854775808\n\t\t\t},\n\t\t\t{\n\t\t\t\t\"file_hash\": \"xRaxgUwgTXYFb16YnR+Q+VVsczLl6jmYwvzhQ/ncrh4=\",\n\t\t\t\t\"seqno\": 5176527,\n\t\t\t\t\"root_hash\": \"SoPLqMe9Dz26YJPOGDOHApTSe5i0kXFtRmRh/zPMGuI=\",\n\t\t\t\t\"workchain\": -1,\n\t\t\t\t\"shard\": -9223372036854775808\n\t\t\t}\n\t\t]\n\t}\n}\n");


  LOG(INFO) << "INIT SERVER TUNNEL...";
  // TODO: pass port_
  auto res = PrepareTunnel(&on_recv_batch, callback_.release(), const_cast<char*>(cfg.data()), cfg.size(), const_cast<char*>(netCfg.data()), netCfg.size());
  LOG(INFO) << "INIT SERVER TUNNEL DONE";
  tunnel_index_ = res.index;

  td:IPAddress ip;
  ip.init_ipv4_port(td::IPAddress::ipv4_to_str(res.ip), static_cast<td::uint16>(res.port)).ensure();
  on_ready_.set_value(std::move(ip));

  alarm_timestamp() = td::Timestamp::in(0.05);
}

void UdpServerTunnelImpl::on_recv_batch(void *next, char *data, size_t num) {
  for (size_t i = 0; i < num; i++) {
    UdpMessage msg;
    msg.address.init_sockaddr(reinterpret_cast<sockaddr *>(data));
    const uint16_t len = (static_cast<uint16_t>(data[16]) << 8) + data[17];
    msg.data = BufferSlice(data+18, len);
    data += 18+len;

    // both init_sockaddr and BufferSlice doing memcpy so it is safe
    static_cast<Callback*>(next)->on_udp_message(std::move(msg));
  }
}

td::actor::ActorOwn<UdpServerTunnelImpl> UdpServerTunnelImpl::create(td::Slice name, int32 port,
                                                                     std::unique_ptr<Callback> callback,
                                                                     td::Promise<td::IPAddress> on_ready) {
  return td::actor::create_actor<UdpServerTunnelImpl>(
      actor::ActorOptions().with_name(name).with_poll(!td::Poll::is_edge_triggered()), port, std::move(callback), std::move(on_ready));
}

UdpServerTunnelImpl::UdpServerTunnelImpl(int32 port, std::unique_ptr<Callback> callback, td::Promise<td::IPAddress> on_ready): port_(port), callback_(std::move(callback)), on_ready_(std::move(on_ready)) {
}

class UdpServerImpl : public UdpServer {
 public:
  void send(td::UdpMessage &&message) override;
  static td::actor::ActorOwn<UdpServerImpl> create(td::Slice name, td::UdpSocketFd fd,
                                                   std::unique_ptr<Callback> callback);

  UdpServerImpl(td::UdpSocketFd fd, std::unique_ptr<Callback> callback);

 private:
  td::actor::ActorOwn<> fd_listener_;
  std::unique_ptr<Callback> callback_;
  td::BufferedUdp fd_;
  bool is_closing_{false};

  void start_up() override;
  void on_fd_updated();

  void loop() override;

  void hangup() override;
  void hangup_shared() override;
};

void UdpServerImpl::send(td::UdpMessage &&message) {
  //LOG(WARNING) << "TO: " << message.address;
  fd_.send(std::move(message));
  loop();  // TODO: some yield logic
}

td::actor::ActorOwn<UdpServerImpl> UdpServerImpl::create(td::Slice name, td::UdpSocketFd fd,
                                                         std::unique_ptr<Callback> callback) {
  return td::actor::create_actor<UdpServerImpl>(
      actor::ActorOptions().with_name(name).with_poll(!td::Poll::is_edge_triggered()), std::move(fd),
      std::move(callback));
}

UdpServerImpl::UdpServerImpl(td::UdpSocketFd fd, std::unique_ptr<Callback> callback)
    : callback_(std::move(callback)), fd_(std::move(fd)) {
}

void UdpServerImpl::start_up() {
  //CHECK(td::actor::SchedulerContext::get()->has_poll() == false);
  class Observer : public td::ObserverBase, public Destructor {
   public:
    Observer(td::actor::ActorShared<UdpServerImpl> udp_server) : udp_server_(std::move(udp_server)) {
    }
    void notify() override {
      VLOG(udp_server) << "on_fd_updated";
      td::actor::send_signals_later(udp_server_, td::actor::ActorSignals::wakeup());
    }

   private:
    td::actor::ActorShared<UdpServerImpl> udp_server_;
  };

  auto observer = std::make_unique<Observer>(actor_shared(this));
  auto pollable_fd = fd_.get_poll_info().extract_pollable_fd(observer.get());
  fd_listener_ = td::actor::create_actor<FdListener>(actor::ActorOptions().with_name("FdListener").with_poll(),
                                                     std::move(pollable_fd), std::move(observer));
}

void UdpServerImpl::on_fd_updated() {
  loop();
}

void UdpServerImpl::loop() {
  if (is_closing_) {
    return;
  }
  //CHECK(td::actor::SchedulerContext::get()->has_poll() == false);
  fd_.get_poll_info().get_flags();
  VLOG(udp_server) << "loop " << td::tag("can read", can_read(fd_)) << " " << td::tag("can write", can_write(fd_));
  Status status;
  status = [&] {
    while (true) {
      TRY_RESULT(o_message, fd_.receive());
      if (!o_message) {
        return Status::OK();
      }
      //LOG(WARNING) << "FROM" << o_message.value().address;
      callback_->on_udp_message(std::move(*o_message));
    }
    return Status::OK();
  }();
  if (status.is_ok()) {
    status = fd_.flush_send();
  }

  if (status.is_error()) {
    VLOG(udp_server) << "Got " << status << " sleep for 1 second";
    alarm_timestamp() = Timestamp::in(1);
  }
}

void UdpServerImpl::hangup() {
  is_closing_ = true;
  // wait till fd_listener_ is closed and fd is unsubscribed
  fd_listener_.reset();
}
void UdpServerImpl::hangup_shared() {
  stop();
}

class TcpClient : public td::actor::Actor, td::ObserverBase {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void on_message(BufferSlice data) = 0;
    virtual void on_closed(actor::ActorId<>) = 0;
  };
  TcpClient(td::SocketFd fd, std::unique_ptr<Callback> callback)
      : buffered_fd_(std::move(fd)), callback_(std::move(callback)) {
  }

  void send(BufferSlice data) {
    uint32 data_size = narrow_cast<uint32>(data.size());

    buffered_fd_.output_buffer().append(Slice(reinterpret_cast<char *>(&data_size), sizeof(data_size)));
    buffered_fd_.output_buffer().append(std::move(data));
    loop();
  }

 private:
  td::BufferedFd<td::SocketFd> buffered_fd_;
  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<TcpClient> self_;

  void notify() override {
    // NB: Interface will be changed
    td::actor::send_closure_later(self_, &TcpClient::on_net);
  }
  void on_net() {
    loop();
  }

  void start_up() override {
    self_ = actor_id(this);
    LOG(INFO) << "Start";
    // Subscribe for socket updates
    // NB: Interface will be changed
    td::actor::SchedulerContext::get()->get_poll().subscribe(buffered_fd_.get_poll_info().extract_pollable_fd(this),
                                                             PollFlags::ReadWrite());
    alarm_timestamp() = Timestamp::in(10);
    notify();
  }

  void tear_down() override {
    LOG(INFO) << "Close";
    // unsubscribe from socket updates
    // nb: interface will be changed
    td::actor::SchedulerContext::get()->get_poll().unsubscribe(buffered_fd_.get_poll_info().get_pollable_fd_ref());
    callback_->on_closed(actor_id(this));
  }

  void loop() override {
    auto status = [&] {
      TRY_STATUS(buffered_fd_.flush_read());
      auto &input = buffered_fd_.input_buffer();
      while (true) {
        constexpr size_t header_size = 4;
        if (input.size() < header_size) {
          break;
        }
        auto it = input.clone();
        uint32 data_size;
        it.advance(header_size, MutableSlice(reinterpret_cast<uint8 *>(&data_size), sizeof(data_size)));
        if (data_size > (1 << 26)) {
          return Status::Error("Too big packet");
        }
        if (it.size() < data_size) {
          break;
        }
        auto data = it.cut_head(data_size).move_as_buffer_slice();
        alarm_timestamp() = Timestamp::in(10);
        callback_->on_message(std::move(data));
        input = std::move(it);
      }

      TRY_STATUS(buffered_fd_.flush_write());
      if (td::can_close(buffered_fd_)) {
        stop();
      }
      return td::Status::OK();
    }();
    if (status.is_error()) {
      LOG(INFO) << "Client got error " << status;
      stop();
    }
  }
  void alarm() override {
    LOG(INFO) << "Close because of timeout";
    stop();
  }
};

struct Target {
  IPAddress ip_address;
  actor::ActorOwn<TcpClient> inbound;
  actor::ActorOwn<TcpClient> outbound;
};

class TargetSet {
 public:
  using Id = size_t;
  Id register_target(IPAddress address) {
    auto it_ok = ip_to_id_.insert(std::make_pair(address, 0));
    if (it_ok.second) {
      id_to_target_.push_back({});
      id_to_target_.back().ip_address = address;
      it_ok.first->second = id_to_target_.size();
    }
    return it_ok.first->second;
  }

  Target &get_target(Id id) {
    return id_to_target_.at(id - 1);
  }

 private:
  std::map<IPAddress, size_t> ip_to_id_;
  std::vector<Target> id_to_target_;
};
class UdpServerViaTcp : public UdpServer {
 public:
  UdpServerViaTcp(int32 port, std::unique_ptr<Callback> callback) : port_(port), callback_(std::move(callback)) {
  }

 private:
  int32 port_;
  std::unique_ptr<Callback> callback_;
  actor::ActorOwn<TcpInfiniteListener> tcp_listener_;
  TargetSet target_set_;
  int refcnt_{0};
  bool close_flag_{false};

  void start_up() override {
    //TcpInfiniteListener
    class TcpListenerCallback : public TcpListener::Callback {
     public:
      TcpListenerCallback(actor::ActorShared<UdpServerViaTcp> parent) : parent_(std::move(parent)) {
      }
      void accept(SocketFd fd) override {
        actor::send_closure(parent_, &UdpServerViaTcp::accept, std::move(fd));
      }

     private:
      actor::ActorShared<UdpServerViaTcp> parent_;
    };
    refcnt_++;
    tcp_listener_ = actor::create_actor<TcpInfiniteListener>(PSLICE() << "TcpInfiniteListener" << port_, port_,
                                                             std::make_unique<TcpListenerCallback>(actor_shared(this)));
  }

  void send(UdpMessage &&message) override {
    if (close_flag_) {
      return;
    }
    auto target_id = target_set_.register_target(message.address);
    auto &target = target_set_.get_target(target_id);
    if (target.inbound.empty() && target.outbound.empty()) {
      auto r_fd = SocketFd::open(target.ip_address);
      if (r_fd.is_error()) {
        LOG(INFO) << r_fd.error();
        return;
      }
      auto fd = r_fd.move_as_ok();
      do_accept(std::move(fd), message.address, false);
    }
    if (!target.inbound.empty()) {
      send_closure_later(target.inbound, &TcpClient::send, std::move(message.data));
    } else if (!target.outbound.empty()) {
      send_closure_later(target.outbound, &TcpClient::send, std::move(message.data));
    }
  }

  void on_message(BufferSlice data) {
    if (close_flag_) {
      return;
    }
    auto token = get_link_token();
    auto &target = target_set_.get_target(narrow_cast<TargetSet::Id>(token));
    UdpMessage message;
    message.address = target.ip_address;
    message.data = std::move(data);
    callback_->on_udp_message(std::move(message));
  }
  void on_closed(actor::ActorId<> id) {
    if (close_flag_) {
      return;
    }
    auto token = get_link_token();
    auto &target = target_set_.get_target(narrow_cast<TargetSet::Id>(token));
    if (target.inbound.get() == id) {
      target.inbound.reset();
    }
    if (target.outbound.get() == id) {
      target.outbound.reset();
    }
  }

  void accept(SocketFd fd) {
    if (close_flag_) {
      return;
    }
    IPAddress ip_address;
    auto status = ip_address.init_peer_address(fd);
    if (status.is_error()) {
      LOG(INFO) << status;
      return;
    }
    do_accept(std::move(fd), ip_address, true);
  }
  void do_accept(SocketFd fd, IPAddress ip_address, bool is_inbound) {
    class TcpClientCallback : public TcpClient::Callback {
     public:
      TcpClientCallback(actor::ActorShared<UdpServerViaTcp> parent) : parent_(std::move(parent)) {
      }
      void on_message(BufferSlice data) override {
        send_closure(parent_, &UdpServerViaTcp::on_message, std::move(data));
      }
      void on_closed(actor::ActorId<> id) override {
        send_closure(parent_, &UdpServerViaTcp::on_closed, std::move(id));
      }

     private:
      actor::ActorShared<UdpServerViaTcp> parent_;
    };

    auto target_id = target_set_.register_target(ip_address);
    auto &target = target_set_.get_target(target_id);
    refcnt_++;
    auto actor = actor::create_actor<TcpClient>(actor::ActorOptions().with_name("TcpClient").with_poll(), std::move(fd),
                                                std::make_unique<TcpClientCallback>(actor_shared(this, target_id)));
    if (is_inbound) {
      target.inbound = std::move(actor);
    } else {
      target.outbound = std::move(actor);
    }
  }

  void hangup() override {
    close_flag_ = true;
    target_set_ = {};
    tcp_listener_ = {};
  }

  void hangup_shared() override {
    refcnt_--;
    if (refcnt_ == 0) {
      stop();
    }
  }

  void loop() override {
  }
};

}  // namespace detail

Result<actor::ActorOwn<UdpServer>> UdpServer::create(td::Slice name, int32 port, std::unique_ptr<Callback> callback) {
  td::IPAddress from_ip;
  TRY_STATUS(from_ip.init_ipv4_port("0.0.0.0", port));
  TRY_RESULT(fd, UdpSocketFd::open(from_ip));
  fd.maximize_rcv_buffer().ensure();
  return detail::UdpServerImpl::create(name, std::move(fd), std::move(callback));
}

Result<actor::ActorOwn<UdpServer>> UdpServer::create_via_tunnel(td::Slice name, int32 port,
                                                                std::unique_ptr<Callback> callback,
                                                                td::Promise<td::IPAddress> on_ready) {
  return detail::UdpServerTunnelImpl::create(name, port, std::move(callback), std::move(on_ready));
}

Result<actor::ActorOwn<UdpServer>> UdpServer::create_via_tcp(td::Slice name, int32 port,
                                                             std::unique_ptr<Callback> callback) {
  return actor::create_actor<detail::UdpServerViaTcp>(name, port, std::move(callback));
}

}  // namespace td
