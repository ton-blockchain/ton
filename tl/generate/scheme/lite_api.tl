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

tonNode.blockId#b7cdb167 workchain:int shard:long seqno:int = tonNode.BlockId;
tonNode.blockIdExt#6752eb78 workchain:int shard:long seqno:int root_hash:int256 file_hash:int256 = tonNode.BlockIdExt;
tonNode.zeroStateIdExt#1d7235ae workchain:int root_hash:int256 file_hash:int256 = tonNode.ZeroStateIdExt;

adnl.message.query#b48bf97a query_id:int256 query:bytes = adnl.Message;
adnl.message.answer#0fac8416 query_id:int256 answer:bytes = adnl.Message;

liteServer.error#bba9e148 code:int message:string = liteServer.Error;

liteServer.accountId#75a0e2c5 workchain:int id:int256 = liteServer.AccountId;
liteServer.libraryEntry#8aff2446 hash:int256 data:bytes = liteServer.LibraryEntry;

liteServer.masterchainInfo#85832881 last:tonNode.blockIdExt state_root_hash:int256 init:tonNode.zeroStateIdExt = liteServer.MasterchainInfo;
liteServer.masterchainInfoExt#a8cce0f5 mode:# version:int capabilities:long last:tonNode.blockIdExt last_utime:int now:int state_root_hash:int256 init:tonNode.zeroStateIdExt = liteServer.MasterchainInfoExt;
liteServer.currentTime#e953000d now:int = liteServer.CurrentTime;
liteServer.version#5a0491e5 mode:# version:int capabilities:long now:int = liteServer.Version;
liteServer.blockData#a574ed6c id:tonNode.blockIdExt data:bytes = liteServer.BlockData;
liteServer.blockState#abaddc0c id:tonNode.blockIdExt root_hash:int256 file_hash:int256 data:bytes = liteServer.BlockState;
liteServer.blockHeader#752d8219 id:tonNode.blockIdExt mode:# header_proof:bytes = liteServer.BlockHeader;
liteServer.sendMsgStatus#3950e597 status:int = liteServer.SendMsgStatus;
liteServer.accountState#7079c751 id:tonNode.blockIdExt shardblk:tonNode.blockIdExt shard_proof:bytes proof:bytes state:bytes = liteServer.AccountState;
liteServer.runMethodResult#a39a616b mode:# id:tonNode.blockIdExt shardblk:tonNode.blockIdExt shard_proof:mode.0?bytes proof:mode.0?bytes state_proof:mode.1?bytes init_c7:mode.3?bytes lib_extras:mode.4?bytes exit_code:int result:mode.2?bytes = liteServer.RunMethodResult;
liteServer.shardInfo#9fe6cd84 id:tonNode.blockIdExt shardblk:tonNode.blockIdExt shard_proof:bytes shard_descr:bytes = liteServer.ShardInfo;
liteServer.allShardsInfo#098fe72d id:tonNode.blockIdExt proof:bytes data:bytes = liteServer.AllShardsInfo;
liteServer.transactionInfo#0edeed47 id:tonNode.blockIdExt proof:bytes transaction:bytes = liteServer.TransactionInfo;
liteServer.transactionList#6f26c60b ids:(vector tonNode.blockIdExt) transactions:bytes = liteServer.TransactionList;
liteServer.transactionId#b12f65af mode:# account:mode.0?int256 lt:mode.1?long hash:mode.2?int256 = liteServer.TransactionId;
liteServer.transactionId3#2c81da77 account:int256 lt:long = liteServer.TransactionId3;
liteServer.blockTransactions#bd8cad2b id:tonNode.blockIdExt req_count:# incomplete:Bool ids:(vector liteServer.transactionId) proof:bytes = liteServer.BlockTransactions;
liteServer.blockTransactionsExt#fb8ffce4 id:tonNode.blockIdExt req_count:# incomplete:Bool transactions:bytes proof:bytes = liteServer.BlockTransactionsExt;
liteServer.signature#a3def855 node_id_short:int256 signature:bytes = liteServer.Signature;
liteServer.signatureSet#f644a6e6 validator_set_hash:int catchain_seqno:int signatures:(vector liteServer.signature) = liteServer.SignatureSet;
liteServer.blockLinkBack#ef7e1bef to_key_block:Bool from:tonNode.blockIdExt to:tonNode.blockIdExt dest_proof:bytes proof:bytes state_proof:bytes = liteServer.BlockLink;
liteServer.blockLinkForward#520fce1c to_key_block:Bool from:tonNode.blockIdExt to:tonNode.blockIdExt dest_proof:bytes config_proof:bytes signatures:liteServer.SignatureSet = liteServer.BlockLink;
liteServer.partialBlockProof#8ed0d2c1 complete:Bool from:tonNode.blockIdExt to:tonNode.blockIdExt steps:(vector liteServer.BlockLink) = liteServer.PartialBlockProof;
liteServer.configInfo#ae7b272f mode:# id:tonNode.blockIdExt state_proof:bytes config_proof:bytes = liteServer.ConfigInfo;
liteServer.validatorStats#b9f796d8 mode:# id:tonNode.blockIdExt count:int complete:Bool state_proof:bytes data_proof:bytes = liteServer.ValidatorStats;
liteServer.libraryResult#117ab96b result:(vector liteServer.libraryEntry) = liteServer.LibraryResult;
liteServer.shardBlockLink#d30dcf72 id:tonNode.blockIdExt proof:bytes = liteServer.ShardBlockLink;
liteServer.shardBlockProof#1d62a07a masterchain_id:tonNode.blockIdExt links:(vector liteServer.shardBlockLink) = liteServer.ShardBlockProof;

liteServer.debug.verbosity#5d404733 value:int = liteServer.debug.Verbosity;

---functions---

liteServer.getMasterchainInfo#89b5e62e = liteServer.MasterchainInfo;
liteServer.getMasterchainInfoExt#70a671df mode:# = liteServer.MasterchainInfoExt;
liteServer.getTime#16ad5a34 = liteServer.CurrentTime;
liteServer.getVersion#232b940b = liteServer.Version;
liteServer.getBlock#6377cf0d id:tonNode.blockIdExt = liteServer.BlockData;
liteServer.getState#ba6e2eb6 id:tonNode.blockIdExt = liteServer.BlockState;
liteServer.getBlockHeader#21ec069e id:tonNode.blockIdExt mode:# = liteServer.BlockHeader;
liteServer.sendMessage#690ad482 body:bytes = liteServer.SendMsgStatus;
liteServer.getAccountState#6b890e25 id:tonNode.blockIdExt account:liteServer.accountId = liteServer.AccountState;
liteServer.getAccountStatePrunned#5a698507 id:tonNode.blockIdExt account:liteServer.accountId = liteServer.AccountState;
liteServer.runSmcMethod#5cc65dd2 mode:# id:tonNode.blockIdExt account:liteServer.accountId method_id:long params:bytes = liteServer.RunMethodResult;
liteServer.getShardInfo#46a2f425 id:tonNode.blockIdExt workchain:int shard:long exact:Bool = liteServer.ShardInfo;
liteServer.getAllShardsInfo#74d3fd6b id:tonNode.blockIdExt = liteServer.AllShardsInfo;
liteServer.getOneTransaction#d40f24ea id:tonNode.blockIdExt account:liteServer.accountId lt:long = liteServer.TransactionInfo;
liteServer.getTransactions#1c40e7a1 count:# account:liteServer.accountId lt:long hash:int256 = liteServer.TransactionList;
liteServer.lookupBlock#fac8f71e mode:# id:tonNode.blockId lt:mode.1?long utime:mode.2?int = liteServer.BlockHeader;
liteServer.listBlockTransactions#adfcc7da id:tonNode.blockIdExt mode:# count:# after:mode.7?liteServer.transactionId3 reverse_order:mode.6?true want_proof:mode.5?true = liteServer.BlockTransactions;
liteServer.listBlockTransactionsExt#0079dd5c id:tonNode.blockIdExt mode:# count:# after:mode.7?liteServer.transactionId3 reverse_order:mode.6?true want_proof:mode.5?true = liteServer.BlockTransactionsExt;
liteServer.getBlockProof#8aea9c44 mode:# known_block:tonNode.blockIdExt target_block:mode.0?tonNode.blockIdExt = liteServer.PartialBlockProof;
liteServer.getConfigAll#911b26b7 mode:# id:tonNode.blockIdExt = liteServer.ConfigInfo;
liteServer.getConfigParams#2a111c19 mode:# id:tonNode.blockIdExt param_list:(vector int) = liteServer.ConfigInfo;
liteServer.getValidatorStats#091a58bc mode:# id:tonNode.blockIdExt limit:int start_after:mode.0?int256 modified_after:mode.2?int = liteServer.ValidatorStats;
liteServer.getLibraries#d122b662 library_list:(vector int256) = liteServer.LibraryResult;
liteServer.getShardBlockProof#4ca60350 id:tonNode.blockIdExt = liteServer.ShardBlockProof;

liteServer.queryPrefix#72d3e686 = Object;
liteServer.query#798c06df data:bytes = Object;
liteServer.waitMasterchainSeqno#baeab892 seqno:int timeout_ms:int = Object; // query prefix
