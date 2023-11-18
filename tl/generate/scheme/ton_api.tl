int ? = Int;
long ? = Long;
double ? = Double;
string ? = String;
object ? = Object;
function ? = Function;
bytes#184614d1 data:string = Bytes;
true#3fedd339 = True;
boolTrue#997275b5 = Bool;
boolFalse#bc799737 = Bool;


vector#1cb5c415 {t:Type} # [ t ] = Vector t;

int128#84ccf7b7 4*[ int ] = Int128;
int256#7bedeb5b 8*[ int ] = Int256;

testObject#a557498a value:int o:object f:function = TestObject;
testString#c84571c9 value:string = TestObject;
testInt#2b9651d1 value:int = TestObject;
testVectorBytes#4b8b1bd3 value:(vector bytes) = TestObject;

tcp.pong#dc69fb03 random_id:long = tcp.Pong;

tcp.authentificate#445bab12 nonce:bytes = tcp.Message;
tcp.authentificationNonce#e35d4ab6 nonce:bytes = tcp.Message;
tcp.authentificationComplete#f7ad9ea6 key:PublicKey signature:bytes = tcp.Message;

fec.raptorQ#8b93a7e0 data_size:int symbol_size:int symbols_count:int = fec.Type;
fec.roundRobin#32f528e4 data_size:int symbol_size:int symbols_count:int = fec.Type;
fec.online#0127660c data_size:int symbol_size:int symbols_count:int = fec.Type;

---functions---

tcp.ping#4d082b9a random_id:long = tcp.Pong;

getTestObject#0bbfa683 = TestObject;

---types---

pk.unenc#b1db9b30 data:bytes = PrivateKey;
pk.ed25519#49682317 key:int256 = PrivateKey;
pk.aes#a5e85137 key:int256 = PrivateKey;
pk.overlay#37a5f65b name:bytes = PrivateKey;

pub.unenc#b61f450a data:bytes = PublicKey;
pub.ed25519#4813b4c6 key:int256 = PublicKey;
pub.aes#2dbcadd4 key:int256 = PublicKey;
pub.overlay#34ba45cb name:bytes = PublicKey;


---functions---

---types---

adnl.id.short#3e3f654f id:int256 = adnl.id.Short;

adnl.proxyToFastHash#ddbdf85e ip:int port:int date:int data_hash:int256 shared_secret:int256 = adnl.ProxyTo;
adnl.proxyToFast#b4ee21d6 ip:int port:int date:int signature:int256 = adnl.ProxyToSign;

adnl.proxy.none#3532487b id:int256 = adnl.Proxy;
adnl.proxy.fast#3a8b45b5 id:int256 shared_secret:bytes = adnl.Proxy;


adnl.address.udp#670da6e7 ip:int port:int = adnl.Address;
adnl.address.udp6#e31d63fa ip:int128 port:int = adnl.Address;
//adnl.address.tcp ip:int port:int = adnl.Address;
//adnl.address.tcp6 ip:int128 port:int = adnl.Address;

adnl.address.tunnel#092b02eb to:int256 pubkey:PublicKey = adnl.Address;
adnl.address.reverse#27795286 = adnl.Address;

adnl.addressList#2227e658 addrs:(vector adnl.Address) version:int reinit_date:int priority:int expire_at:int = adnl.AddressList;

adnl.node#6b561285 id:PublicKey addr_list:adnl.addressList = adnl.Node;
adnl.nodes#a209db56 nodes:(vector adnl.node) = adnl.Nodes;

---functions---

---types---

adnl.packetContents#d142cd89
  rand1:bytes 
  flags:# 
  from:flags.0?PublicKey 
  from_short:flags.1?adnl.id.short
  message:flags.2?adnl.Message 
  messages:flags.3?(vector adnl.Message)
  address:flags.4?adnl.addressList 
  priority_address:flags.5?adnl.addressList
  seqno:flags.6?long 
  confirm_seqno:flags.7?long 
  recv_addr_list_version:flags.8?int
  recv_priority_addr_list_version:flags.9?int
  reinit_date:flags.10?int 
  dst_reinit_date:flags.10?int
  signature:flags.11?bytes 
  rand2:bytes 
        = adnl.PacketContents;

adnl.tunnelPacketContents#c59138b4
  rand1:bytes 
  flags:# 
  from_ip:flags.0?int
  from_port:flags.0?int
  message:flags.1?bytes 
  statistics:flags.2?bytes
  payment:flags.3?bytes
  rand2:bytes 
        = adnl.TunnelPacketContents;


// flag 16 - packet is outbound
// flag 17 - control packet
adnl.proxyPacketHeader#08693c78
  proxy_id:int256
  flags:# 
  ip:flags.0?int
  port:flags.0?int
  adnl_start_time:flags.1?int
  seqno:flags.2?long
  date:flags.3?int
  signature:int256 = adnl.ProxyPacketHeader;

adnl.proxyControlPacketPing#3796e44b id:int256 = adnl.ProxyControlPacket;
adnl.proxyControlPacketPong#4bd1dbfc id:int256 = adnl.ProxyControlPacket;
adnl.proxyControlPacketRegister#c309b23f ip:int port:int = adnl.ProxyControlPacket;


adnl.message.createChannel#e673c3bb key:int256 date:int = adnl.Message;
adnl.message.confirmChannel#60dd1d69 key:int256 peer_key:int256 date:int = adnl.Message;

adnl.message.custom#204818f5 data:bytes = adnl.Message;

adnl.message.nop#17f8dfda = adnl.Message;
adnl.message.reinit#10c20520 date:int = adnl.Message;

adnl.message.query#b48bf97a query_id:int256 query:bytes = adnl.Message;
adnl.message.answer#0fac8416 query_id:int256 answer:bytes = adnl.Message;

adnl.message.part#fd452d39 hash:int256 total_size:int offset:int data:bytes = adnl.Message;

---functions---
---types---

adnl.db.node.key#c5a3e42e local_id:int256 peer_id:int256 = adnl.db.Key;
adnl.db.node.value#545d2707 date:int id:PublicKey addr_list:adnl.addressList priority_addr_list:adnl.addressList = adnl.db.node.Value;

---functions---


---types---

rldp2.messagePart#11480b6e transfer_id:int256 fec_type:fec.Type part:int total_size:long seqno:int data:bytes = rldp2.MessagePart;
rldp2.confirm#23e69945 transfer_id:int256 part:int max_seqno:int received_mask:int received_count:int = rldp2.MessagePart;
rldp2.complete#36b9081f transfer_id:int256 part:int = rldp2.MessagePart;

rldp.messagePart#185c22cc transfer_id:int256 fec_type:fec.Type part:int total_size:long seqno:int data:bytes = rldp.MessagePart;
rldp.confirm#f582dc58 transfer_id:int256 part:int seqno:int = rldp.MessagePart;
rldp.complete#bc0cb2bf transfer_id:int256 part:int = rldp.MessagePart;

rldp.message#7d1bcd1e id:int256 data:bytes = rldp.Message;
rldp.query#8a794d69 query_id:int256 max_answer_size:long timeout:int data:bytes = rldp.Message;
rldp.answer#a3fc5c03 query_id:int256 data:bytes = rldp.Message;


---functions---
---types---
dht.node#84533248 id:PublicKey addr_list:adnl.addressList version:int signature:bytes = dht.Node;
dht.nodes#7974a0be nodes:(vector dht.node) = dht.Nodes;

dht.key#f667de8f id:int256 name:bytes idx:int = dht.Key;

dht.updateRule.signature#cc9f31f7 = dht.UpdateRule;
dht.updateRule.anybody#61578e14 = dht.UpdateRule;
dht.updateRule.overlayNodes#26779383 = dht.UpdateRule;

dht.keyDescription#281d4e05 key:dht.key id:PublicKey update_rule:dht.UpdateRule signature:bytes = dht.KeyDescription;

dht.value#90ad27cb key:dht.keyDescription value:bytes ttl:int signature:bytes = dht.Value;

dht.pong#5a8aef81 random_id:long = dht.Pong;

dht.valueNotFound#a2620568 nodes:dht.nodes = dht.ValueResult;
dht.valueFound#e40cf774 value:dht.Value = dht.ValueResult;

dht.clientNotFound#2d1c7e6f nodes:dht.nodes = dht.ReversePingResult;
dht.reversePingOk#204030a2 = dht.ReversePingResult;

dht.stored#7026fb08 = dht.Stored;
dht.message#bc0cdb8e node:dht.node = dht.Message;
dht.requestReversePingCont#dbadc105 target:adnl.Node signature:bytes client:int256 = dht.RequestReversePingCont;

dht.db.bucket#b39cfa6c nodes:dht.nodes = dht.db.Bucket;
dht.db.key.bucket#a368ae4c id:int = dht.db.Key;

---functions---

dht.ping#cbeb3f18 random_id:long = dht.Pong;
dht.store#34934212 value:dht.value = dht.Stored;
dht.findNode#6ce2ce6b key:int256 k:int = dht.Nodes;
dht.findValue#ae4b6011 key:int256 k:int = dht.ValueResult;
dht.getSignedAddressList#a97948ed = dht.Node;
dht.registerReverseConnection#222cbc61 node:PublicKey ttl:int signature:bytes = dht.Stored;
dht.requestReversePing#0b94a40a target:adnl.Node signature:bytes client:int256 k:int = dht.ReversePingResult;

dht.query#7d530769 node:dht.node = True;

---types---

overlay.node.toSign#03d8a8e1 id:adnl.id.short overlay:int256 version:int = overlay.node.ToSign;
overlay.node#b86b8a83 id:PublicKey overlay:int256 version:int signature:bytes = overlay.Node;
overlay.nodes#e487290e nodes:(vector overlay.node) = overlay.Nodes;

overlay.message#75252420 overlay:int256 = overlay.Message;
//overlay.randomPeers peers:(vector adnl.node) = overlay.RandomPeers;
overlay.broadcastList#18d1dedf hashes:(vector int256) = overlay.BroadcastList;

overlay.fec.received#d55c14ec hash:int256 = overlay.Broadcast;
overlay.fec.completed#09d76914 hash:int256 = overlay.Broadcast;

overlay.broadcast.id#51fd789a src:int256 data_hash:int256 flags:int = overlay.broadcast.Id;
overlay.broadcastFec.id#fb3155a6 src:int256 type:int256 data_hash:int256 size:int flags:int = overlay.broadcastFec.Id;
overlay.broadcastFec.partId#a46962d0 broadcast_hash:int256 data_hash:int256 seqno:int = overlay.broadcastFec.PartId;

overlay.broadcast.toSign#fa374e7c hash:int256 date:int = overlay.broadcast.ToSign;

overlay.certificate#e09ed731 issued_by:PublicKey expire_at:int max_size:int signature:bytes = overlay.Certificate;
overlay.certificateV2#b43f9c83 issued_by:PublicKey expire_at:int max_size:int flags:int signature:bytes = overlay.Certificate;
overlay.emptyCertificate#32dabccf = overlay.Certificate;

overlay.certificateId#8fae60b9 overlay_id:int256 node:int256 expire_at:int max_size:int = overlay.CertificateId;
overlay.certificateIdV2#fc6cd2a7 overlay_id:int256 node:int256 expire_at:int max_size:int flags:int = overlay.CertificateId;

overlay.unicast#33534e24 data:bytes = overlay.Broadcast;
overlay.broadcast#b15a2b6b src:PublicKey certificate:overlay.Certificate flags:int data:bytes date:int signature:bytes = overlay.Broadcast;
overlay.broadcastFec#bad7c36a src:PublicKey certificate:overlay.Certificate data_hash:int256 data_size:int flags:int
          data:bytes seqno:int fec:fec.Type date:int signature:bytes = overlay.Broadcast;
overlay.broadcastFecShort#f1881342 src:PublicKey certificate:overlay.Certificate broadcast_hash:int256 part_data_hash:int256 seqno:int signature:bytes = overlay.Broadcast;
overlay.broadcastNotFound#95863624 = overlay.Broadcast;

---functions---

overlay.getRandomPeers#48ee64ab peers:overlay.nodes = overlay.Nodes;

overlay.query#ccfd8443 overlay:int256 = True;
overlay.getBroadcast#2d35f2a0 hash:int256 = overlay.Broadcast;
overlay.getBroadcastList#421c283a list:overlay.broadcastList = overlay.BroadcastList;

---types---

overlay.db.nodes#d588ce1a nodes:overlay.nodes = overlay.db.Nodes;
overlay.db.key.nodes#c4d07316 local_id:int256 overlay:int256 = overlay.db.Key;

---functions---

---types---

catchain.block.id#24fe98ba incarnation:int256 src:int256 height:int data_hash:int256 = catchain.block.Id;
catchain.block.dep#5a1ad14f src:int height:int data_hash:int256 signature:bytes = catchain.block.Dep;
catchain.block.data#f8aca620 prev:catchain.block.dep deps:(vector catchain.block.dep) = catchain.block.Data;
catchain.block#d6554174 incarnation:int256 src:int height:int data:catchain.block.data signature:bytes = catchain.Block;
catchain.blocks#50ecd1c1 blocks:(vector catchain.block) = catchain.Blocks;
catchain.blockUpdate#236758c4 block:catchain.block = catchain.Update;

catchain.block.data.badBlock#b6025a56 block:catchain.block = catchain.block.inner.Data;
catchain.block.data.fork#647a3a52 left:catchain.block.Dep right:catchain.block.Dep = catchain.block.inner.Data;
catchain.block.data.nop#5482b4d0 = catchain.block.inner.Data;
//catchain.block.data.vector msgs:(vector bytes) = catchain.block.inner.Data;
//catchain.block.data.custom = catchain.block.inner.Data;

catchain.firstblock#10c904fb unique_hash:int256 nodes:(vector int256) = catchain.FirstBlock;

catchain.difference#1415d1ca sent_upto:(vector int) = catchain.Difference;
catchain.differenceFork#4927c06f left:catchain.block.dep right:catchain.block.dep = catchain.Difference;

catchain.blockNotFound#b6110884 = catchain.BlockResult;
catchain.blockResult#9d2a3047 block:catchain.block = catchain.BlockResult;

---functions---

catchain.getBlock#093ddd78 block:int256 = catchain.BlockResult;
catchain.getDifference#d06cced8 rt:(vector int) = catchain.Difference;
//catchain.getForkDifference src:int fork:catchain.fork = catchain.ForkDifference;

---types---

validatorSession.round.id#0025cfa5 session:int256 height:long prev_block:int256 seqno:int = validatorSession.round.Id;

validatorSession.candidate.id#bcd74139 round:int256 block_hash:int256 = validatorSession.tempBlock.Id;

validatorSession.message.startSession#96a166d1 = validatorSession.Message;
validatorSession.message.finishSession#cb9b22e3 = validatorSession.Message;

validatorSession.message.submittedBlock#127624b6 round:int root_hash:int256 file_hash:int256
               collated_data_file_hash:int256 = validatorSession.round.Message;
validatorSession.message.approvedBlock#04a5b581 round:int candidate:int256 signature:bytes = validatorSession.round.Message;
validatorSession.message.rejectedBlock#95884e6b round:int candidate:int256 reason:bytes = validatorSession.round.Message;
validatorSession.message.commit#ac129ef5 round:int candidate:int256 signature:bytes = validatorSession.round.Message;

validatorSession.message.vote#9a3251c7 round:int attempt:int candidate:int256 = validatorSession.round.Message;
validatorSession.message.voteFor#61f0fe2f round:int attempt:int candidate:int256 = validatorSession.round.Message;
validatorSession.message.precommit#a854b552 round:int attempt:int candidate:int256 = validatorSession.round.Message;
validatorSession.message.empty#4a201fa9 round:int attempt:int = validatorSession.round.Message;

validatorSession.pong#dcc6376d hash:long = validatorSession.Pong;

validatorSession.candidateId#19fee56c src:int256 root_hash:int256 file_hash:int256 collated_data_file_hash:int256 = validatorSession.CandidateId;

validatorSession.blockUpdate#9283ce37 ts:long actions:(vector validatorSession.round.Message) state:int = validatorSession.BlockUpdate;
validatorSession.candidate#7d337845 src:int256 round:int root_hash:int256 data:bytes collated_data:bytes = validatorSession.Candidate;

validatorSession.config#b661fdc3 catchain_idle_timeout:double catchain_max_deps:int round_candidates:int next_candidate_delay:double round_attempt_duration:int
        max_round_attempts:int max_block_size:int max_collated_data_size:int = validatorSession.Config;
validatorSession.configNew#f7afa99c catchain_idle_timeout:double catchain_max_deps:int round_candidates:int next_candidate_delay:double round_attempt_duration:int
        max_round_attempts:int max_block_size:int max_collated_data_size:int new_catchain_ids:Bool = validatorSession.Config;
validatorSession.configVersioned#402a9703 catchain_idle_timeout:double catchain_max_deps:int round_candidates:int next_candidate_delay:double round_attempt_duration:int
        max_round_attempts:int max_block_size:int max_collated_data_size:int version:int = validatorSession.Config;

validatorSession.catchainOptions#70e249e6 idle_timeout:double max_deps:int max_block_size:int block_hash_covers_data:Bool
        max_block_height_ceoff:int debug_disable_db:Bool = validatorSession.CatChainOptions;
validatorSession.configVersionedV2#a97b11af catchain_opts:validatorSession.CatChainOptions round_candidates:int next_candidate_delay:double
        round_attempt_duration:int max_round_attempts:int max_block_size:int max_collated_data_size:int version:int = validatorSession.Config;

---functions---

validatorSession.ping#680449ad hash:long = validatorSession.Pong;
validatorSession.downloadCandidate#e0fd3df5 round:int id:validatorSession.candidateId = validatorSession.Candidate;

---types---

hashable.bool#cf61441c value:Bool = Hashable;
hashable.int32#d3b59356 value:int = Hashable;
hashable.int64#e7da8e42 value:long = Hashable;
hashable.int256#3a2313cf value:int256 = Hashable;
hashable.bytes#0713de12 value:bytes = Hashable;
hashable.pair#c7e56895 left:int right:int = Hashable;
hashable.vector#df34c36d value:(vector int) = Hashable;
hashable.validatorSessionOldRound#478b67a9 seqno:int block:int signatures:int approve_signatures:int = Hashable;
hashable.validatorSessionRoundAttempt#4c11ffad seqno:int votes:int precommitted:int vote_for_inited:int vote_for:int = Hashable;
hashable.validatorSessionRound#35774fe3 locked_round:int locked_block:int seqno:int precommitted:Bool
          first_attempt:int approved_blocks:int signatures:int attempts:int = Hashable;
hashable.blockSignature#37e192a2 signature:int = Hashable;
hashable.sentBlock#bdb9952b src:int root_hash:int file_hash:int collated_data_file_hash:int = Hashable;
hashable.sentBlockEmpty#9ef246af = Hashable;
hashable.vote#aebf2bc5 block:int node:int = Hashable;
hashable.blockCandidate#0ba9b10d block:int approved:int = Hashable;
hashable.blockVoteCandidate#cf0d6fe5 block:int approved:int = Hashable;
hashable.blockCandidateAttempt#3f5c7d0b block:int votes:int = Hashable;

hashable.cntVector#0b286f38 data:int = Hashable;
hashable.cntSortedVector#7b964659 data:int = Hashable;

hashable.validatorSession#681263d5 ts:int old_rounds:int cur_round:int = Hashable;

---functions---
---types---


tonNode.sessionId#7a9236ba workchain:int shard:long cc_seqno:int opts_hash:int256 = tonNode.SessionId;


tonNode.blockSignature#50f03c33 who:int256 signature:bytes = tonNode.BlockSignature;

tonNode.blockId#b7cdb167 workchain:int shard:long seqno:int = tonNode.BlockId;
tonNode.blockIdExt#6752eb78 workchain:int shard:long seqno:int root_hash:int256 file_hash:int256 = tonNode.BlockIdExt;
tonNode.zeroStateIdExt#1d7235ae workchain:int root_hash:int256 file_hash:int256 = tonNode.ZeroStateIdExt;

tonNode.blockDescriptionEmpty#8384ae95 = tonNode.BlockDescription;
tonNode.blockDescription#46a1d088 id:tonNode.blockIdExt = tonNode.BlockDescription;
tonNode.blocksDescription#d62a612c ids:(vector tonNode.blockIdExt) incomplete:Bool = tonNode.BlocksDescription;
tonNode.preparedProofEmpty#c769c17a = tonNode.PreparedProof;
tonNode.preparedProof#899f9a4b = tonNode.PreparedProof;
tonNode.preparedProofLink#3dff328d = tonNode.PreparedProof;
tonNode.preparedState#375bcb6d = tonNode.PreparedState;
tonNode.notFoundState#32390a51 = tonNode.PreparedState;
tonNode.prepared#eac4bbcd = tonNode.Prepared;
tonNode.notFound#e2c33da6 = tonNode.Prepared;
tonNode.data#560a2484 data:bytes = tonNode.Data;
//tonNode.preparedKeyBlockProofEmpty = tonNode.PreparedKeyBlockProof;
//tonNode.preparedKeyBlockProof block_id:tonNode.blockIdExt = tonNode.PreparedKeyBlockProof;

tonNode.ihrMessage#4534c307 data:bytes = tonNode.IhrMessage;
tonNode.externalMessage#dc75a209 data:bytes = tonNode.ExternalMessage;

tonNode.newShardBlock#a49dc229 block:tonNode.blockIdExt cc_seqno:int data:bytes = tonNode.NewShardBlock;

tonNode.blockBroadcast#ae2e1105 id:tonNode.blockIdExt catchain_seqno:int validator_set_hash:int
              signatures:(vector tonNode.blockSignature) 
              proof:bytes data:bytes = tonNode.Broadcast;
tonNode.ihrMessageBroadcast#525da4b3 message:tonNode.ihrMessage = tonNode.Broadcast;
tonNode.externalMessageBroadcast#3d1b1867 message:tonNode.externalMessage = tonNode.Broadcast;
tonNode.newShardBlockBroadcast#0af2fabc block:tonNode.newShardBlock = tonNode.Broadcast;

tonNode.shardPublicOverlayId#4d9ed329 workchain:int shard:long zero_state_file_hash:int256 = tonNode.ShardPublicOverlayId;

tonNode.keyBlocks#07664d59 blocks:(vector tonNode.blockIdExt) incomplete:Bool error:Bool = tonNode.KeyBlocks;

ton.blockId#c50b6e70 root_cell_hash:int256 file_hash:int256 = ton.BlockId;
ton.blockIdApprove#2dd44a49 root_cell_hash:int256 file_hash:int256 = ton.BlockId;

tonNode.dataList#14f43313 data:(vector bytes) = tonNode.DataList;

tonNode.dataFull#be589f93 id:tonNode.blockIdExt proof:bytes block:bytes is_link:Bool = tonNode.DataFull;
tonNode.dataFullEmpty#576e85ca = tonNode.DataFull;

tonNode.capabilities#f5bf60c0 version:int capabilities:long = tonNode.Capabilities;

tonNode.success#c096244f = tonNode.Success;

tonNode.archiveNotFound#99291683 = tonNode.ArchiveInfo;
tonNode.archiveInfo#19efff8c id:long = tonNode.ArchiveInfo;

---functions---

tonNode.getNextBlockDescription#1455b0f3 prev_block:tonNode.blockIdExt = tonNode.BlockDescription;
tonNode.getNextBlocksDescription#3f2812c4 prev_block:tonNode.blockIdExt limit:int = tonNode.BlocksDescription;
tonNode.getPrevBlocksDescription#5c6d6cc9 next_block:tonNode.blockIdExt limit:int cutoff_seqno:int = tonNode.BlocksDescription;
tonNode.prepareBlockProof#875c3308 block:tonNode.blockIdExt allow_partial:Bool = tonNode.PreparedProof;
tonNode.prepareKeyBlockProof#77364c38 block:tonNode.blockIdExt allow_partial:Bool = tonNode.PreparedProof;
tonNode.prepareBlockProofs#ed79b2b8 blocks:(vector tonNode.blockIdExt) allow_partial:Bool = tonNode.PreparedProof;
tonNode.prepareKeyBlockProofs#8c6cfbe4 blocks:(vector tonNode.blockIdExt) allow_partial:Bool = tonNode.PreparedProof;
tonNode.prepareBlock#75a37f4e block:tonNode.blockIdExt = tonNode.Prepared;
tonNode.prepareBlocks#6affabfc blocks:(vector tonNode.blockIdExt) = tonNode.Prepared;
tonNode.preparePersistentState#feea269e block:tonNode.blockIdExt masterchain_block:tonNode.blockIdExt = tonNode.PreparedState;
tonNode.prepareZeroState#41ce0825 block:tonNode.blockIdExt = tonNode.PreparedState;
tonNode.getNextKeyBlockIds#f2e7cfbb block:tonNode.blockIdExt max_size:int = tonNode.KeyBlocks;
tonNode.downloadNextBlockFull#6ea0374a prev_block:tonNode.blockIdExt = tonNode.DataFull;
tonNode.downloadBlockFull#6a27c49d block:tonNode.blockIdExt = tonNode.DataFull;
tonNode.downloadBlock#e27279c3 block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadBlocks#7659c57d blocks:(vector tonNode.blockIdExt) = tonNode.DataList;
tonNode.downloadPersistentState#7f99e3b8 block:tonNode.blockIdExt masterchain_block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadPersistentStateSlice#f5e9e6e3 block:tonNode.blockIdExt masterchain_block:tonNode.blockIdExt offset:long max_size:long = tonNode.Data;
tonNode.downloadZeroState#adcc1e5a block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadBlockProof#4bd6478a block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadKeyBlockProof#ec23483a block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadBlockProofs#a5b053f5 blocks:(vector tonNode.blockIdExt) = tonNode.DataList;
tonNode.downloadKeyBlockProofs#c327de3a blocks:(vector tonNode.blockIdExt) = tonNode.DataList;
tonNode.downloadBlockProofLink#25b300c6 block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadKeyBlockProofLink#12e42ad2 block:tonNode.blockIdExt = tonNode.Data;
tonNode.downloadBlockProofLinks#28d12b63 blocks:(vector tonNode.blockIdExt) = tonNode.DataList;
tonNode.downloadKeyBlockProofLinks#75c38550 blocks:(vector tonNode.blockIdExt) = tonNode.DataList;
tonNode.getArchiveInfo#7b2dd941 masterchain_seqno:int = tonNode.ArchiveInfo;
tonNode.getArchiveSlice#203b5168 archive_id:long offset:long max_size:int = tonNode.Data;

tonNode.getCapabilities#dee618f8 = tonNode.Capabilities;

tonNode.slave.sendExtMessage#0376f2a9 message:tonNode.externalMessage = tonNode.Success;

tonNode.query#69f324d3 = Object;

---types---

// bit 0 - started
// bit 1 - ready to switch
// bit 2 - switched from
// bit 3 - archived
// bit 4 - disabled

db.root.dbDescription#b41873f3 version:int first_masterchain_block_id:tonNode.blockIdExt flags:int = db.root.DbDescription;

db.root.key.cellDb#72f9b33e version:int = db.root.Key;
db.root.key.blockDb#3012bf40 version:int = db.root.Key;

db.root.config#d61182a1 celldb_version:int blockdb_version:int = db.root.Config;
db.root.key.config#13c33284 = db.root.Key;

db.celldb.value#e6101440 block_id:tonNode.blockIdExt prev:int256 next:int256 root_hash:int256 = db.celldb.Value;
db.celldb.key.value#5bb13923 hash:int256 = db.celldb.key.Value;

db.block.info#4ac6e727 id:tonNode.blockIdExt flags:# prev_left:flags.1?tonNode.blockIdExt
                                            prev_right:flags.2?tonNode.blockIdExt
                                            next_left:flags.3?tonNode.blockIdExt
                                            next_right:flags.4?tonNode.blockIdExt
                                            lt:flags.13?long 
                                            ts:flags.14?int
                                            state:flags.17?int256 
                                            masterchain_ref_seqno:flags.23?int = db.block.Info;
db.block.packedInfo#46bb9192 id:tonNode.blockIdExt unixtime:int offset:long = db.block.Info;
db.block.archivedInfo#205f7a51 id:tonNode.blockIdExt flags:# next:flags.0?tonNode.blockIdExt = db.block.Info;

db.blockdb.value#b28ec42d next:tonNode.blockIdExt data:bytes = db.blockdb.Value;
db.blockdb.lru#c11655b3 id:tonNode.blockIdExt prev:int256 next:int256 = db.blockdb.Lru;
db.blockdb.key.lru#50bc963a id:tonNode.blockIdExt = db.blockdb.Key;
db.blockdb.key.value#7f57d173 id:tonNode.blockIdExt = db.blockdb.Key;

db.candidate#65d96ada source:PublicKey id:tonNode.blockIdExt data:bytes collated_data:bytes = db.Candidate;
db.candidate.id#37c0b287 source:PublicKey id:tonNode.blockIdExt collated_data_file_hash:int256 = db.candidate.Id;

db.filedb.key.empty#7bff274b = db.filedb.Key;
db.filedb.key.blockFile#b0eae471 block_id:tonNode.blockIdExt = db.filedb.Key;
db.filedb.key.zeroStateFile#1252863d block_id:tonNode.blockIdExt = db.filedb.Key;
db.filedb.key.persistentStateFile#afb6764c block_id:tonNode.blockIdExt masterchain_block_id:tonNode.blockIdExt = db.filedb.Key;
db.filedb.key.proof#da954dec block_id:tonNode.blockIdExt = db.filedb.Key;
db.filedb.key.proofLink#98fbc5ce block_id:tonNode.blockIdExt = db.filedb.Key;
db.filedb.key.signatures#d7290d0b block_id:tonNode.blockIdExt = db.filedb.Key;
db.filedb.key.candidate#e28a0ab9 id:db.candidate.id = db.filedb.Key;
db.filedb.key.blockInfo#c499d4fc block_id:tonNode.blockIdExt = db.filedb.Key;

db.filedb.value#f2dd1a2d key:db.filedb.Key prev:int256 next:int256 file_hash:int256 = db.filedb.Value;

db.state.destroyedSessions#ada8d984 sessions:(vector int256) = db.state.DestroyedSessions;
db.state.initBlockId#732c9cf5 block:tonNode.blockIdExt = db.state.InitBlockId;
db.state.gcBlockId#df30bd4f block:tonNode.blockIdExt = db.state.GcBlockId;
db.state.shardClient#0b16a69d block:tonNode.blockIdExt = db.state.ShardClient;
db.state.asyncSerializer#d32f29a1 block:tonNode.blockIdExt last:tonNode.blockIdExt last_ts:int = db.state.AsyncSerializer;
db.state.hardforks#85f30d04 blocks:(vector tonNode.blockIdExt) = db.state.Hardforks;
db.state.dbVersion#d93720f7 version:int = db.state.DbVersion;

db.state.key.destroyedSessions#e8f7f159 = db.state.Key;
db.state.key.initBlockId#758278e3 = db.state.Key;
db.state.key.gcBlockId#c379f3de = db.state.Key;
db.state.key.shardClient#c99b3187 = db.state.Key;
db.state.key.asyncSerializer#29ae8a1f = db.state.Key;
db.state.key.hardforks#e6f427ba = db.state.Key;
db.state.key.dbVersion#724f2154 = db.state.Key;

db.lt.el.key#a5321ae2 workchain:int shard:long idx:int = db.lt.Key;
db.lt.desc.key#f1e3e791 workchain:int shard:long = db.lt.Key;
db.lt.shard.key#50a6f90f idx:int = db.lt.Key;
db.lt.status.key#776c6057 = db.lt.Key;
db.lt.el.value#95e65f64 id:tonNode.blockIdExt lt:long ts:int = db.lt.el.Value;
db.lt.desc.value#71af51b4 first_idx:int last_idx:int last_seqno:int last_lt:long last_ts:int = db.lt.desc.Value;
db.lt.shard.value#3c739a7b workchain:int shard:long = db.lt.shard.Value;
db.lt.status.value#fabeed39 total_shards:int = db.lt.status.Value;

db.files.index.key#7dc40502 = db.files.Key;
db.files.package.key#a504033e package_id:int key:Bool temp:Bool = db.files.Key;

db.files.index.value#a2b1dafc packages:(vector int) key_packages:(vector int) temp_packages:(vector int) = db.files.index.Value;
db.files.package.firstBlock#701269e7 workchain:int shard:long seqno:int unixtime:int lt:long = db.files.package.FirstBlock;
db.files.package.value#e44cd52b package_id:int key:Bool temp:Bool firstblocks:(vector db.files.package.firstBlock) deleted:Bool
                   = db.files.package.Value;

---functions---

---types---

validator.groupMember#8b9465e4 public_key_hash:int256 adnl:int256 weight:long = engine.validator.GroupMember;
validator.group#f8d87ea1 workchain:int shard:long catchain_seqno:int config_hash:int256 members:(vector validator.groupMember) = validator.Group;
validator.groupEx#1c924dfe workchain:int shard:long vertical_seqno:int catchain_seqno:int config_hash:int256 members:(vector validator.groupMember) = validator.Group;
validator.groupNew#9843a14d workchain:int shard:long vertical_seqno:int last_key_block_seqno:int catchain_seqno:int config_hash:int256 members:(vector validator.groupMember) = validator.Group;

---functions---


---types---


id.config.local#92a9c78e id:PrivateKey = id.config.Local;
dht.config.local#76204a6f id:adnl.id.short = dht.config.Local;
dht.config.random.local#9beb2577 cnt:int = dht.config.Local;
liteserver.config.local#4673eb8f id:PrivateKey port:int = liteserver.config.Local;
liteserver.config.random.local#7cc9453b port:int = liteserver.config.Local;
validator.config.local#664bff68 id:adnl.id.short = validator.config.Local;
validator.config.random.local#59839462 addr_list:adnl.addressList = validator.config.Local;
control.config.local#751deced priv:PrivateKey pub:int256 port:int = control.config.Local;
config.local#789e915c local_ids:(vector id.config.local) dht:(vector dht.config.Local) validators:(vector validator.config.Local) liteservers:(vector liteserver.config.Local) control:(vector control.config.local) = config.Local;

dht.config.global#84ceca07 static_nodes:dht.nodes k:int a:int = dht.config.Global;
dht.config.global_v2#69638427 static_nodes:dht.nodes k:int a:int network_id:int = dht.config.Global;
adnl.config.global#be6f80d0 static_nodes:adnl.nodes = adnl.config.Global;
catchain.config.global#68c7b651 tag:int256 nodes:(vector PublicKey) = catchain.config.Global;
dummyworkchain0.config.global#da616ed3 zero_state_hash:int256 = dummyworkchain0.config.Global;
validator.config.global#867dff6a zero_state:tonNode.blockIdExt init_block:tonNode.blockIdExt hardforks:(vector tonNode.blockIdExt) = validator.config.Global;
config.global#f066e9b0 adnl:adnl.config.global dht:dht.config.Global validator:validator.config.global = config.Global;

liteserver.desc#c449a474 id:PublicKey ip:int port:int = liteserver.Desc;
liteclient.config.global#088dc0f8 liteservers:(vector liteserver.desc) validator:validator.config.global = liteclient.config.Global;

engine.adnl#62d76550 id:int256 category:int = engine.Adnl;
engine.addr#ef311fec ip:int port:int categories:(vector int) priority_categories:(vector int) = engine.Addr;
engine.addrProxy#8adf6549 in_ip:int in_port:int out_ip:int out_port:int
          proxy_type:adnl.Proxy categories:(vector int) priority_categories:(vector int) = engine.Addr;
engine.dht#5de9f2fa id:int256 = engine.Dht;
engine.validatorTempKey#5e4ad6de key:int256 expire_at:int = engine.ValidatorTempKey;
engine.validatorAdnlAddress#d34545be id:int256 expire_at:int = engine.ValidatorAdnlAddress;
engine.validator#885fea29 id:int256 temp_keys:(vector engine.validatorTempKey) adnl_addrs:(vector engine.validatorAdnlAddress) election_date:int expire_at:int = engine.Validator;
engine.liteServer#bb708efe id:int256 port:int = engine.LiteServer;
engine.controlProcess#6ac04817 id:int256 permissions:int = engine.ControlProcess;
engine.controlInterface#31816fab id:int256 port:int allowed:(vector engine.controlProcess) = engine.ControlInterface;
engine.gc#bfbd987b ids:(vector int256) = engine.Gc;

engine.dht.config#f43d80c6 dht:(vector engine.dht) gc:engine.gc = engine.dht.Config;
engine.validator.fullNodeMaster#8485f668 port:int adnl:int256 = engine.validator.FullNodeMaster;
engine.validator.fullNodeSlave#88256b79 ip:int port:int adnl:PublicKey = engine.validator.FullNodeSlave;
engine.validator.fullNodeConfig#29feb114 ext_messages_broadcast_disabled:Bool = engine.validator.FullNodeConfig;
engine.validator.config#e81f1ed0 out_port:int addrs:(vector engine.Addr) adnl:(vector engine.adnl)
        dht:(vector engine.dht)
        validators:(vector engine.validator) fullnode:int256 fullnodeslaves:(vector engine.validator.fullNodeSlave)
        fullnodemasters:(vector engine.validator.fullNodeMaster)
        fullnodeconfig:engine.validator.fullNodeConfig
        liteservers:(vector engine.liteServer) control:(vector engine.controlInterface)
        gc:engine.gc = engine.validator.Config;

---functions---
---types---

engine.adnlProxy.port#f901754a in_port:int out_port:int dst_ip:int dst_port:int proxy_type:adnl.Proxy = engine.adnlProxy.Port;

engine.adnlProxy.config#6e264101 ports:(vector engine.adnlProxy.port) = engine.adnlProxy.Config;

---functions---

---types---

adnl.pong#20747c0e value:long = adnl.Pong;

---functions---

adnl.ping#1faaa1bf value:long = adnl.Pong;

---types---

engine.validator.keyHash#c2c6a54e key_hash:int256 = engine.validator.KeyHash;
engine.validator.signature#fb6c4328 signature:bytes = engine.validator.Signature;

engine.validator.oneStat#a4983aed key:string value:string = engine.validator.OneStat;
engine.validator.stats#5d49d36f stats:(vector engine.validator.oneStat) = engine.validator.Stats;

engine.validator.controlQueryError#77269a1f code:int message:string = engine.validator.ControlQueryError;

engine.validator.time#df5fa1fe time:int = engine.validator.Time;
engine.validator.success#b3e4a68b = engine.validator.Success;

engine.validator.jsonConfig#132d920b data:string = engine.validator.JsonConfig;

engine.validator.electionBid#23b27a3d election_date:int perm_key:int256 adnl_addr:int256 to_send_payload:bytes = engine.validator.ElectionBid;
engine.validator.proposalVote#7f6626ed perm_key:int256 to_send:bytes = engine.validator.ProposalVote;

engine.validator.dhtServerStatus#b11de75e id:int256 status:int = engine.validator.DhtServerStatus;
engine.validator.dhtServersStatus#2b38fd28 servers:(vector engine.validator.dhtServerStatus) = engine.validator.DhtServersStatus;

engine.validator.overlayStatsNode#f97220d9 adnl_id:int256 ip_addr:string bdcst_errors:int fec_bdcst_errors:int last_in_query:int last_out_query:int t_out_bytes:int t_in_bytes:int t_out_pckts:int t_in_pckts:int = engine.validator.OverlayStatsNode;

engine.validator.overlayStats#dfa0faf9 overlay_id:int256 overlay_id_full:PublicKey adnl_id:int256 scope:string nodes:(vector engine.validator.overlayStatsNode) stats:(vector engine.validator.oneStat) = engine.validator.OverlayStats;
engine.validator.overlaysStats#9c09267f overlays:(vector engine.validator.overlayStats) = engine.validator.OverlaysStats;

engine.validator.onePerfTimerStat#9123a368 time:int min:double avg:double max:double = engine.validator.OnePerfTimerStat;
engine.validator.perfTimerStatsByName#82bacde4 name:string stats:(vector engine.validator.OnePerfTimerStat) = engine.validator.PerfTimerStatsByName;
engine.validator.perfTimerStats#5fd0551b stats:(vector engine.validator.PerfTimerStatsByName) = engine.validator.PerfTimerStats;

engine.validator.shardOutQueueSize#0fdba45d size:int = engine.validator.ShardOutQueueSize;


---functions---

engine.validator.getTime#e140bed1 = engine.validator.Time;
engine.validator.importPrivateKey#15807ac7 key:PrivateKey = engine.validator.KeyHash;
engine.validator.exportPrivateKey#cc728048 key_hash:int256 = PrivateKey;
engine.validator.exportPublicKey#6234a8b9 key_hash:int256 = PublicKey;
engine.validator.generateKeyPair#eb25607b = engine.validator.KeyHash;
engine.validator.addAdnlId#ed8554ab key_hash:int256 category:int = engine.validator.Success;
engine.validator.addDhtId#f50c1e8c key_hash:int256 = engine.validator.Success;
engine.validator.addValidatorPermanentKey#92150578 key_hash:int256 election_date:int ttl:int = engine.validator.Success;
engine.validator.addValidatorTempKey#8d336f32 permanent_key_hash:int256 key_hash:int256 ttl:int = engine.validator.Success;
engine.validator.addValidatorAdnlAddress#dacba682 permanent_key_hash:int256 key_hash:int256 ttl:int = engine.validator.Success;
engine.validator.changeFullNodeAdnlAddress#bec6c985 adnl_id:int256 = engine.validator.Success;
engine.validator.addLiteserver#f08a0f47 key_hash:int256 port:int = engine.validator.Success;
engine.validator.addControlInterface#348bf3fc key_hash:int256 port:int = engine.validator.Success;
engine.validator.addControlProcess#5ae0f750 key_hash:int256 port:int peer_key:int256 permissions:int = engine.validator.Success;

engine.validator.delAdnlId#293a74f2 key_hash:int256 = engine.validator.Success;
engine.validator.delDhtId#84fd5b3e key_hash:int256 = engine.validator.Success;
engine.validator.delValidatorPermanentKey#174ac8fa key_hash:int256 = engine.validator.Success;
engine.validator.delValidatorTempKey#a0e6e0d1 permanent_key_hash:int256 key_hash:int256 = engine.validator.Success;
engine.validator.delValidatorAdnlAddress#f708435a permanent_key_hash:int256 key_hash:int256 = engine.validator.Success;

engine.validator.addListeningPort#ea6b89b5 ip:int port:int categories:(vector int) priority_categories:(vector int) = engine.validator.Success;
engine.validator.addProxy#f6fd33f5 in_ip:int in_port:int out_ip:int out_port:int proxy:adnl.Proxy categories:(vector int) priority_categories:(vector int) = engine.validator.Success;
engine.validator.delListeningPort#315bb84f ip:int port:int categories:(vector int) priority_categories:(vector int) = engine.validator.Success;
engine.validator.delProxy#7578cc7d out_ip:int out_port:int categories:(vector int) priority_categories:(vector int) = engine.validator.Success;

engine.validator.sign#1aea1a28 key_hash:int256 data:bytes = engine.validator.Signature;

engine.validator.getStats#52d5c311 = engine.validator.Stats;
engine.validator.getConfig#59ad2225 = engine.validator.JsonConfig;

engine.validator.setVerbosity#b1825e82 verbosity:int = engine.validator.Success;

engine.validator.createElectionBid#e51db145 election_date:int election_addr:string wallet:string = engine.validator.ElectionBid;
engine.validator.createProposalVote#1db3216d vote:bytes = engine.validator.ProposalVote;
engine.validator.createComplaintVote#b083ff2a election_id:int vote:bytes = engine.validator.ProposalVote;

engine.validator.checkDhtServers#d1e420ca id:int256 = engine.validator.DhtServersStatus;

engine.validator.getOverlaysStats#fcd8acce = engine.validator.OverlaysStats;

engine.validator.controlQuery#a476bdc0 data:bytes = Object;

engine.validator.importCertificate#3c82c0cf overlay_id:int256 local_id:adnl.id.short signed_key:engine.validator.KeyHash cert:overlay.Certificate = engine.validator.Success;
engine.validator.signShardOverlayCertificate#5c973c56 workchain:int shard:long signed_key:engine.validator.KeyHash expire_at:int max_size:int = overlay.Certificate;
engine.validator.importShardOverlayCertificate#1ac30a58 workchain:int shard:long signed_key:engine.validator.KeyHash cert:overlay.Certificate = engine.validator.Success;

engine.validator.getPerfTimerStats#ea42f8ef name:string = engine.validator.PerfTimerStats;
engine.validator.getShardOutQueueSize#5ba15c50 flags:# block_id:tonNode.blockId dest_wc:flags.0?int dest_shard:flags.0?long = engine.validator.ShardOutQueueSize;
engine.validator.setExtMessagesBroadcastDisabled#8a9109da disabled:Bool = engine.validator.Success;

---types---

storage.pong#6cf5c6a5 = storage.Pong;
storage.ok#c32b1c05 = Ok;

storage.state#3313708a will_upload:Bool want_download:Bool = storage.State;
storage.piece#80b4fa0d proof:bytes data:bytes = storage.Piece;
storage.torrentInfo#14ced0ee data:bytes = storage.TorrentInfo;

storage.updateInit#ce33e0b6 have_pieces:bytes have_pieces_offset:int state:storage.State = storage.Update;
storage.updateHavePieces#3bf82049 piece_id:(vector int) = storage.Update;
storage.updateState#05b034b5 state:storage.State = storage.Update;

---functions---

storage.ping#44f3f211 session_id:long = storage.Pong;
storage.addUpdate#4d3135d2 session_id:long seqno:int update:storage.Update = Ok;

storage.getTorrentInfo#91c4962a = storage.TorrentInfo;
storage.getPiece#807ae660 piece_id:int = storage.Piece;

---types---

http.header#8e9be511 name:string value:string = http.Header;
http.payloadPart#295ad764 data:bytes trailer:(vector http.header) last:Bool = http.PayloadPart;
http.response#ca48a74a http_version:string status_code:int reason:string headers:(vector http.header) no_payload:Bool = http.Response;
http.proxy.capabilities#31926c11 capabilities:long = http.proxy.Capabilities;

---functions---

http.request#61b191e1 id:int256 method:string url:string http_version:string headers:(vector http.header) = http.Response;
http.getNextPayloadPart#90745d0c id:int256 seqno:int max_chunk_size:int = http.PayloadPart;
http.proxy.getCapabilities#db721f89 capabilities:long = http.proxy.Capabilities;

---types---


http.server.dnsEntry#d8726096 domain:string addr:adnl.id.short = http.server.DnsEntry;
http.server.host#c57de2a7 domains:(vector string) ip:int port:int adnl_id:adnl.id.short = http.server.Host;

http.server.config#3a1477fc dhs:(vector http.server.dnsEntry) local_hosts:(vector http.server.host) = http.server.Config;

---functions---

---types---

validatorSession.statsProducer#8dfba60d id:int256 block_status:int block_timestamp:long = validatorSession.StatsProducer;

validatorSession.statsRound#a32fdf66 timestamp:long producers:(vector validatorSession.statsProducer) = validatorSession.StatsRound;

validatorSession.stats#684d5a8f id:tonNode.blockId timestamp:long self:int256 creator:int256 total_validators:int total_weight:long
    signatures:int signatures_weight:long approve_signatures:int approve_signatures_weight:long
    first_round:int rounds:(vector validatorSession.statsRound) = validatorSession.Stats;

---functions---

---types---

storage.db.key.torrentList#cbc6e856 = storage.db.key.TorrentList;
storage.db.key.torrent#b988122f hash:int256 = storage.db.key.TorrentShort;
storage.db.key.torrentMeta#62239d66 hash:int256 = storage.db.key.TorrentMeta;
storage.db.key.priorities#b5f1ca6d hash:int256 = storage.db.key.Priorities;
storage.db.key.piecesInDb#dbe8c9e3 hash:int256 = storage.db.key.PiecesInDb;
storage.db.key.pieceInDb#c40aedcd hash:int256 idx:long = storage.db.key.PieceInDb;
storage.db.key.config#dd983407 = storage.db.key.Config;

storage.db.config#5d904d28 flags:# download_speed_limit:double upload_speed_limit:double = storage.db.Config;
storage.db.torrentList#59efe381 torrents:(vector int256) = storage.db.TorrentList;
storage.db.torrent#ad2d379a root_dir:string active_download:Bool active_upload:Bool = storage.db.TorrentShort;
storage.db.torrentV2#89ecf89b flags:# root_dir:string added_at:int active_download:Bool active_upload:Bool = storage.db.TorrentShort;
storage.db.priorities#3929eb4e actions:(vector storage.PriorityAction) = storage.db.Priorities;
storage.db.piecesInDb#0619b43c pieces:(vector long) = storage.db.PiecesInDb;

storage.priorityAction.all#fe238940 priority:int = storage.PriorityAction;
storage.priorityAction.idx#950fb728 idx:long priority:int = storage.PriorityAction;
storage.priorityAction.name#0124d1c0 name:string priority:int = storage.PriorityAction;

storage.daemon.config#f0c694b7 server_key:PublicKey cli_key_hash:int256 provider_address:string adnl_id:PublicKey dht_id:PublicKey = storage.daemon.provider.Config;

storage.daemon.provider.params#ac736e07 accept_new_contracts:Bool rate_per_mb_day:string max_span:int
    minimal_file_size:long maximal_file_size:long = storage.daemon.provider.Params;

storage.provider.db.key.state#f420cfa2 = storage.provider.db.key.State;
storage.provider.db.key.contractList#5592cc46 = storage.provider.db.key.ContractList;
storage.provider.db.key.storageContract#cc5ecb1e wc:int addr:int256 = storage.provider.db.key.StorageContract;
storage.provider.db.key.microchunkTree#2998ea79 wc:int addr:int256 = storage.provider.db.key.MicrochunkTree;
storage.provider.db.key.providerConfig#e7e80932 = storage.provider.db.key.ProviderConfig;

storage.provider.db.state#01a955f3 last_processed_lt:long = storage.provider.db.State;
storage.provider.db.contractAddress#e258ecfd wc:int addr:int256 = storage.db.ContractAddress;
storage.provider.db.contractList#da38e717 contracts:(vector storage.provider.db.contractAddress) = storage.db.ContractList;
storage.provider.db.storageContract#eeb3a732 torrent_hash:int256 microchunk_hash:int256 created_time:int state:int file_size:long
    rate:string max_span:int = storage.provider.db.StorageContract;
storage.provider.db.microchunkTree#c2ca0f42 data:bytes = storage.provider.db.MicrochunkTree;

storage.daemon.queryError#04bdbac4 message:string = storage.daemon.QueryError;
storage.daemon.success#b3aeef1c = storage.daemon.Success;

storage.daemon.torrent#15b30b67
    hash:int256 flags:#
    // 0 - info ready
    // 1 - header ready
    // 2 - fatal error
    total_size:flags.0?long description:flags.0?string
    files_count:flags.1?long included_size:flags.1?long dir_name:flags.1?string
    downloaded_size:long
    added_at:int root_dir:string active_download:Bool active_upload:Bool completed:Bool
    download_speed:double upload_speed:double
    fatal_error:flags.2?string
    = storage.daemon.Torrent;

storage.daemon.fileInfo#7177dbfe
    name:string size:long flags:#
    priority:int
    downloaded_size:long
    = storage.daemon.FileInfo;

storage.daemon.torrentFull#5fa88c27 torrent:storage.daemon.torrent files:(vector storage.daemon.fileInfo) = storage.daemon.TorrentFull;
storage.daemon.torrentList#4f1c1842 torrents:(vector storage.daemon.torrent) = storage.daemon.TorrentList;
storage.daemon.torrentMeta#d4be1ee8 meta:bytes = storage.daemon.TorrentMeta;

storage.daemon.filePiecesInfo#03553543 name:string range_l:long range_r:long = storage.daemon.FilePiecesInfo;
storage.daemon.torrentPiecesInfo#091cab91
    flags:# // 0 - with file ranges
    total_pieces:long piece_size:int
    range_l:long range_r:long piece_ready_bitset:bytes
    files:flags.0?(vector storage.daemon.filePiecesInfo) // files[0] is header
    = storage.daemon.TorrentPiecesInfo;

storage.daemon.newContractParams#555b0884 rate:string max_span:int = storage.daemon.NewContractParams;
storage.daemon.newContractParamsAuto#af7767ad provider_address:string = storage.daemon.NewContractParams;
storage.daemon.newContractMessage#f589adf6 body:bytes rate:string max_span:int = storage.daemon.NewContractMessage;

storage.daemon.peer#bd345034 adnl_id:int256 ip_str:string download_speed:double upload_speed:double ready_parts:long = storage.daemon.Peer;
storage.daemon.peerList#a5de3815 peers:(vector storage.daemon.peer) download_speed:double upload_speed:double total_parts:long = storage.daemon.PeerList;

storage.daemon.prioritySet#b6e89fd7 = storage.daemon.SetPriorityStatus;
storage.daemon.priorityPending#840961a6 = storage.daemon.SetPriorityStatus;

storage.daemon.keyHash#85b562dc key_hash:int256 = storage.daemon.KeyHash;

storage.daemon.speedLimits#feb0e919 download:double upload:double = storage.daemon.SpeedLimits;

storage.daemon.providerConfig#7dad0a94 max_contracts:int max_total_size:long = storage.daemon.ProviderConfig;
storage.daemon.contractInfo#d7744f09 address:string state:int torrent:int256 created_time:int file_size:long downloaded_size:long
    rate:string max_span:int client_balance:string contract_balance:string = storage.daemon.ContractInfo;
storage.daemon.providerInfo#e76b012d address:string balance:string config:storage.daemon.providerConfig
    contracts_count:int contracts_total_size:long
    contracts:(vector storage.daemon.contractInfo) = storage.daemon.ProviderInfo;
storage.daemon.providerAddress#885eb912 address:string = storage.daemon.ProviderAddress;

---functions---
storage.daemon.setVerbosity#26bcbb98 verbosity:int = storage.daemon.Success;
storage.daemon.createTorrent#9d99bc2b path:string description:string allow_upload:Bool copy_inside:Bool flags:# = storage.daemon.TorrentFull;
storage.daemon.addByHash#b535689e hash:int256 root_dir:string start_download:Bool allow_upload:Bool priorities:(vector storage.PriorityAction) flags:# = storage.daemon.TorrentFull;
storage.daemon.addByMeta#b659165e meta:bytes root_dir:string start_download:Bool allow_upload:Bool priorities:(vector storage.PriorityAction) flags:# = storage.daemon.TorrentFull;
storage.daemon.setActiveDownload#747a5d9d hash:int256 active:Bool = storage.daemon.Success;
storage.daemon.setActiveUpload#3baeb69b hash:int256 active:Bool = storage.daemon.Success;

storage.daemon.getTorrents#2335fb5a flags:# = storage.daemon.TorrentList;
storage.daemon.getTorrentFull#5c9a4066 hash:int256 flags:# = storage.daemon.TorrentFull;
storage.daemon.getTorrentMeta#735d2df1 hash:int256 flags:# = storage.daemon.TorrentMeta;
storage.daemon.getNewContractMessage#e9abff94 hash:int256 query_id:long params:storage.daemon.NewContractParams = storage.daemon.NewContractMessage;
storage.daemon.getTorrentPeers#11b7d099 hash:int256 flags:# = storage.daemon.PeerList;
storage.daemon.getTorrentPiecesInfo#f3acb726 hash:int256
    flags:# // 0 - with file ranges
    offset:long max_pieces:long
    = storage.daemon.TorrentPiecesInfo;

storage.daemon.setFilePriorityAll#8d7aa279 hash:int256 priority:int = storage.daemon.SetPriorityStatus;
storage.daemon.setFilePriorityByIdx#4397d69b hash:int256 idx:long priority:int = storage.daemon.SetPriorityStatus;
storage.daemon.setFilePriorityByName#de2d22c9 hash:int256 name:string priority:int = storage.daemon.SetPriorityStatus;

storage.daemon.removeTorrent#0a7a545b hash:int256 remove_files:Bool = storage.daemon.Success;
storage.daemon.loadFrom#c81fde27 hash:int256 meta:bytes path:string flags:# = storage.daemon.Torrent;

storage.daemon.getSpeedLimits#9aae907f flags:# = storage.daemon.SpeedLimits;
storage.daemon.setSpeedLimits#2ba1e7ea flags:# download:flags.0?double upload:flags.1?double = storage.daemon.Success;


storage.daemon.importPrivateKey#7fff7bfa key:PrivateKey = storage.daemon.KeyHash;
storage.daemon.initProvider#281f5acc account_address:string = storage.daemon.Success;
storage.daemon.deployProvider#7db1e7e5 = storage.daemon.ProviderAddress;
storage.daemon.getProviderParams#92cd9671 address:string = storage.daemon.provider.Params;
storage.daemon.setProviderParams#604f2d1c params:storage.daemon.provider.params = storage.daemon.Success;
storage.daemon.getProviderInfo#333d79db with_balances:Bool with_contracts:Bool = storage.daemon.ProviderInfo;
storage.daemon.setProviderConfig#8dacb78c config:storage.daemon.providerConfig = storage.daemon.Success;
storage.daemon.withdraw#fdbcbff1 contract:string = storage.daemon.Success;
storage.daemon.sendCoins#01457726 address:string amount:string message:string = storage.daemon.Success;
storage.daemon.closeStorageContract#f677ab10 address:string = storage.daemon.Success;
storage.daemon.removeStorageProvider#42db9f96 = storage.daemon.Success;
