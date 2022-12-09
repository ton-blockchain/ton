int ? = Int;
long ? = Long;
double ? = Double;
string ? = String;
object ? = Object;
function ? = Function;
bytes data:string = Bytes;
true = True;
boolTrue = Bool;
boolFalse = Bool;


vector {t:Type} # [ t ] = Vector t;

int128 4*[ int ] = Int128;
int256 8*[ int ] = Int256;

tonNode.blockId#67b1cdb7 workchain:int shard:long seqno:int = tonNode.BlockId;
tonNode.blockIdExt#78eb5267 workchain:int shard:long seqno:int root_hash:int256 file_hash:int256 = tonNode.BlockIdExt;
tonNode.zeroStateIdExt#ae35721d workchain:int root_hash:int256 file_hash:int256 = tonNode.ZeroStateIdExt;

adnl.message.query#7af98bb4 query_id:int256 query:bytes = adnl.Message;
adnl.message.answer#1684ac0f query_id:int256 answer:bytes = adnl.Message;

liteServer.error#48e1a9bb code:int message:string = liteServer.Error;

liteServer.accountId#c5e2a075 workchain:int id:int256 = liteServer.AccountId;
liteServer.libraryEntry#4624ff8a hash:int256 data:bytes = liteServer.LibraryEntry;

liteServer.masterchainInfo#81288385 last:tonNode.blockIdExt state_root_hash:int256 init:tonNode.zeroStateIdExt = liteServer.MasterchainInfo;
liteServer.masterchainInfoExt#f5e0cca8 mode:# version:int capabilities:long last:tonNode.blockIdExt last_utime:int now:int state_root_hash:int256 init:tonNode.zeroStateIdExt = liteServer.MasterchainInfoExt;
liteServer.currentTime#d0053e9 now:int = liteServer.CurrentTime;
liteServer.version#e591045a mode:# version:int capabilities:long now:int = liteServer.Version;
liteServer.blockData#6ced74a5 id:tonNode.blockIdExt data:bytes = liteServer.BlockData;
liteServer.blockState#cdcadab id:tonNode.blockIdExt root_hash:int256 file_hash:int256 data:bytes = liteServer.BlockState;
liteServer.blockHeader#19822d75 id:tonNode.blockIdExt mode:# header_proof:bytes = liteServer.BlockHeader;
liteServer.sendMsgStatus#97e55039 status:int = liteServer.SendMsgStatus;
liteServer.accountState#51c77970 id:tonNode.blockIdExt shardblk:tonNode.blockIdExt shard_proof:bytes proof:bytes state:bytes = liteServer.AccountState;
liteServer.runMethodResult#6b619aa3 mode:# id:tonNode.blockIdExt shardblk:tonNode.blockIdExt shard_proof:mode.0?bytes proof:mode.0?bytes state_proof:mode.1?bytes init_c7:mode.3?bytes lib_extras:mode.4?bytes exit_code:int result:mode.2?bytes = liteServer.RunMethodResult;
liteServer.shardInfo#84cde69f id:tonNode.blockIdExt shardblk:tonNode.blockIdExt shard_proof:bytes shard_descr:bytes = liteServer.ShardInfo;
liteServer.allShardsInfo#2de78f09 id:tonNode.blockIdExt proof:bytes data:bytes = liteServer.AllShardsInfo;
liteServer.transactionInfo#47edde0e id:tonNode.blockIdExt proof:bytes transaction:bytes = liteServer.TransactionInfo;
liteServer.transactionList#0bc6266f ids:(vector tonNode.blockIdExt) transactions:bytes = liteServer.TransactionList;
liteServer.transactionId#af652fb1 mode:# account:mode.0?int256 lt:mode.1?long hash:mode.2?int256 = liteServer.TransactionId;
liteServer.transactionId3#77da812c account:int256 lt:long = liteServer.TransactionId3;
liteServer.blockTransactions#5c6c542f id:tonNode.blockIdExt req_count:# incomplete:Bool ids:(vector liteServer.transactionId) proof:bytes = liteServer.BlockTransactions;
liteServer.signature#55f8dea3 node_id_short:int256 signature:bytes = liteServer.Signature;
liteServer.signatureSet#9755e192 validator_set_hash:int catchain_seqno:int signatures:(vector liteServer.signature) = liteServer.SignatureSet;
liteServer.blockLinkBack#ef1b7eef to_key_block:Bool from:tonNode.blockIdExt to:tonNode.blockIdExt dest_proof:bytes proof:bytes state_proof:bytes = liteServer.BlockLink;
liteServer.blockLinkForward#1cce0f52 to_key_block:Bool from:tonNode.blockIdExt to:tonNode.blockIdExt dest_proof:bytes config_proof:bytes signatures:liteServer.SignatureSet = liteServer.BlockLink;
liteServer.partialBlockProof#c1d2d08e complete:Bool from:tonNode.blockIdExt to:tonNode.blockIdExt steps:(vector liteServer.BlockLink) = liteServer.PartialBlockProof;
liteServer.configInfo#2f277bae mode:# id:tonNode.blockIdExt state_proof:bytes config_proof:bytes = liteServer.ConfigInfo;
liteServer.validatorStats#d896f7b9 mode:# id:tonNode.blockIdExt count:int complete:Bool state_proof:bytes data_proof:bytes = liteServer.ValidatorStats;
liteServer.libraryResult#0c43848b result:(vector liteServer.libraryEntry) = liteServer.LibraryResult;
liteServer.shardBlockLink#72cf0dd3 id:tonNode.blockIdExt proof:bytes = liteServer.ShardBlockLink;
liteServer.shardBlockProof#70347608 masterchain_id:tonNode.blockIdExt links:(vector liteServer.shardBlockLink) = liteServer.ShardBlockProof;

liteServer.debug.verbosity#3347405d value:int = liteServer.debug.Verbosity;

---functions---

liteServer.getMasterchainInfo#2ee6b589 = liteServer.MasterchainInfo;
liteServer.getMasterchainInfoExt#df71a670 mode:# = liteServer.MasterchainInfoExt;
liteServer.getTime#345aad16 = liteServer.CurrentTime;
liteServer.getVersion#b942b23 = liteServer.Version;
liteServer.getBlock#0dcf7763 id:tonNode.blockIdExt = liteServer.BlockData;
liteServer.getState#b62e6eba id:tonNode.blockIdExt = liteServer.BlockState;
liteServer.getBlockHeader#9e06ec21 id:tonNode.blockIdExt mode:# = liteServer.BlockHeader;
liteServer.sendMessage#82d40a69 body:bytes = liteServer.SendMsgStatus;
liteServer.getAccountState#250e896b id:tonNode.blockIdExt account:liteServer.accountId = liteServer.AccountState;
liteServer.runSmcMethod#d25dc65c mode:# id:tonNode.blockIdExt account:liteServer.accountId method_id:long params:bytes = liteServer.RunMethodResult;
liteServer.getShardInfo#25f4a246 id:tonNode.blockIdExt workchain:int shard:long exact:Bool = liteServer.ShardInfo;
liteServer.getAllShardsInfo#6bfdd374 id:tonNode.blockIdExt = liteServer.AllShardsInfo;
liteServer.getOneTransaction#ea240fd4 id:tonNode.blockIdExt account:liteServer.accountId lt:long = liteServer.TransactionInfo;
liteServer.getTransactions#a1e7401c count:# account:liteServer.accountId lt:long hash:int256 = liteServer.TransactionList;
liteServer.lookupBlock#1ef7c8fa mode:# id:tonNode.blockId lt:mode.1?long utime:mode.2?int = liteServer.BlockHeader;
liteServer.listBlockTransactions#dac7fcad id:tonNode.blockIdExt mode:# count:# after:mode.7?liteServer.transactionId3 reverse_order:mode.6?true want_proof:mode.5?true = liteServer.BlockTransactions;
liteServer.getBlockProof#449cea8a mode:# known_block:tonNode.blockIdExt target_block:mode.0?tonNode.blockIdExt = liteServer.PartialBlockProof;
liteServer.getConfigAll#b7261b91 mode:# id:tonNode.blockIdExt = liteServer.ConfigInfo;
liteServer.getConfigParams#638df89e mode:# id:tonNode.blockIdExt param_list:(vector int) = liteServer.ConfigInfo;
liteServer.getValidatorStats#091a58bc mode:# id:tonNode.blockIdExt limit:int start_after:mode.0?int256 modified_after:mode.2?int = liteServer.ValidatorStats;
liteServer.getLibraries#99181e7e library_list:(vector int256) = liteServer.LibraryResult;
liteServer.getShardBlockProof#5003a64c id:tonNode.blockIdExt = liteServer.ShardBlockProof;

liteServer.queryPrefix#86e6d372 = Object;
liteServer.query#df068c79 data:bytes = Object;
liteServer.waitMasterchainSeqno#92b8eaba seqno:int timeout_ms:int = Object; // query prefix
