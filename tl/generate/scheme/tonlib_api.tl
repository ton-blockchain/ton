double ? = Double;
string ? = String;

int32 = Int32;
int53 = Int53;
int64 = Int64;
int256#9da18c3c 8*[ int32 ] = Int256;
bytes = Bytes;
secureString#8ea8c283 = SecureString;
secureBytes#bad71dc7 = SecureBytes;

object ? = Object;
function ? = Function;

boolFalse#bc799737 = Bool;
boolTrue#997275b5 = Bool;

vector#1cb5c415 {t:Type} # [ t ] = Vector t;

error#9bdd8f1a code:int32 message:string = Error;
ok#d4edbe69 = Ok;

keyStoreTypeDirectory#e969122a directory:string = KeyStoreType;
keyStoreTypeInMemory#826c09c7 = KeyStoreType;

config#a44e0238 config:string blockchain_name:string use_callbacks_for_network:Bool ignore_cache:Bool = Config;

options#8d4c29f9 config:config keystore_type:KeyStoreType = Options;
options.configInfo#07b75f16 default_wallet_id:int64 default_rwallet_init_public_key:string = options.ConfigInfo;
options.info#fc251c80 config_info:options.configInfo = options.Info;

key#8a1493d5 public_key:string secret:secureBytes = Key;
inputKeyRegular#dee5469e key:key local_password:secureBytes = InputKey;
inputKeyFake#bffb39be = InputKey;
exportedKey#a99e39d7 word_list:vector<secureString> = ExportedKey;
exportedPemKey#54f700bd pem:secureString = ExportedPemKey;
exportedEncryptedKey#78a9fe54 data:secureBytes = ExportedEncryptedKey;
exportedUnencryptedKey#2b839ae8 data:secureBytes = ExportedUnencryptedKey;

bip39Hints#3c559c00 words:vector<string> = Bip39Hints;

adnlAddress#0431950c adnl_address:string = AdnlAddress;

accountAddress#2d09bdab account_address:string = AccountAddress;

unpackedAccountAddress#70d41436 workchain_id:int32 bounceable:Bool testnet:Bool addr:bytes = UnpackedAccountAddress;

internal.transactionId#c5050322 lt:int64 hash:bytes = internal.TransactionId;

ton.blockId#b9587fa2 workchain:int32 shard:int64 seqno:int32 = internal.BlockId;
ton.blockIdExt#7910fc9a workchain:int32 shard:int64 seqno:int32 root_hash:bytes file_hash:bytes = ton.BlockIdExt;

raw.fullAccountState#a8a7cb8f balance:int64 code:bytes data:bytes last_transaction_id:internal.transactionId block_id:ton.blockIdExt frozen_hash:bytes sync_utime:int53 = raw.FullAccountState;
raw.message#518b724f source:accountAddress destination:accountAddress value:int64 fwd_fee:int64 ihr_fee:int64 created_lt:int64 body_hash:bytes msg_data:msg.Data = raw.Message;
raw.transaction#8e689f5a address:accountAddress utime:int53 data:bytes transaction_id:internal.transactionId fee:int64 storage_fee:int64 other_fee:int64 in_msg:raw.message out_msgs:vector<raw.message> = raw.Transaction;
raw.transactions#84fae8ed transactions:vector<raw.transaction> previous_transaction_id:internal.transactionId = raw.Transactions;

raw.extMessageInfo#34197fae hash:bytes = raw.ExtMessageInfo;

pchan.config#8486f436 alice_public_key:string alice_address:accountAddress bob_public_key:string bob_address:accountAddress init_timeout:int32 close_timeout:int32 channel_id:int64 = pchan.Config;

raw.initialAccountState#ebdb5c47 code:bytes data:bytes = InitialAccountState;
wallet.v3.initialAccountState#f8f65540 public_key:string wallet_id:int64 = InitialAccountState;
wallet.v4.initialAccountState#d0e9c2c5 public_key:string wallet_id:int64 = InitialAccountState;
wallet.highload.v1.initialAccountState#ec749e46 public_key:string wallet_id:int64 = InitialAccountState;
wallet.highload.v2.initialAccountState#75347929 public_key:string wallet_id:int64 = InitialAccountState;

rwallet.limit#48def67e seconds:int32 value:int64 = rwallet.Limit;
rwallet.config#fae7849a start_at:int53 limits:vector<rwallet.limit> = rwallet.Config;
rwallet.initialAccountState#45b90c14 init_public_key:string public_key:string wallet_id:int64 = InitialAccountState;

dns.initialAccountState#6dcba4bf public_key:string wallet_id:int64 = InitialAccountState;
pchan.initialAccountState#b23e1d44 config:pchan.config = InitialAccountState;


raw.accountState#e04b963a code:bytes data:bytes frozen_hash:bytes = AccountState;
wallet.v3.accountState#9f7aa84a wallet_id:int64 seqno:int32 = AccountState;
wallet.v4.accountState#d42e9776 wallet_id:int64 seqno:int32 = AccountState;
wallet.highload.v1.accountState#6057e4dc wallet_id:int64 seqno:int32 = AccountState;
wallet.highload.v2.accountState#947d5d4f wallet_id:int64 = AccountState;
dns.accountState#66fad86a wallet_id:int64 = AccountState;
rwallet.accountState#d3eb83d8 wallet_id:int64 seqno:int32 unlocked_balance:int64 config:rwallet.config = AccountState;

pchan.stateInit#b92a0cf8 signed_A:Bool signed_B:Bool min_A:int64 min_B:int64 expire_at:int53 A:int64 B:int64 = pchan.State;
pchan.stateClose#34e201f3 signed_A:Bool signed_B:Bool min_A:int64 min_B:int64 expire_at:int53 A:int64 B:int64 = pchan.State;
pchan.statePayout#279e1447 A:int64 B:int64 = pchan.State;

pchan.accountState#60226f78 config:pchan.config state:pchan.State description:string = AccountState;
uninited.accountState#5abd9708 frozen_hash:bytes = AccountState;

fullAccountState#56d23a49 address:accountAddress balance:int64 last_transaction_id:internal.transactionId block_id:ton.blockIdExt sync_utime:int53 account_state:AccountState revision:int32 = FullAccountState;

accountRevisionList#1f6c64ca revisions:vector<fullAccountState> = AccountRevisionList;
accountList#783eb255 accounts:vector<fullAccountState> = AccountList;

syncStateDone#53f33909 = SyncState;
syncStateInProgress#066bc4c7 from_seqno:int32 to_seqno:int32 current_seqno:int32 = SyncState;

//
// MSG
//

msg.dataRaw#8d065d76 body:bytes init_state:bytes = msg.Data;
msg.dataText#eba43290 text:bytes = msg.Data;
msg.dataDecryptedText#b32960b9 text:bytes = msg.Data;
msg.dataEncryptedText#ee520bda text:bytes = msg.Data;

msg.dataEncrypted#21a13d51 source:accountAddress data:msg.Data = msg.DataEncrypted;
msg.dataDecrypted#0ba960e9 proof:bytes data:msg.Data = msg.DataDecrypted;

msg.dataEncryptedArray#244759b2 elements:vector<msg.dataEncrypted> = msg.DataEncryptedArray;
msg.dataDecryptedArray#e35c4709 elements:vector<msg.dataDecrypted> = msg.DataDecryptedArray;

msg.message#3027b074 destination:accountAddress public_key:string amount:int64 data:msg.Data send_mode:int32 = msg.Message;

//
// DNS
//

dns.entryDataUnknown#b35ad380 bytes:bytes = dns.EntryData;
dns.entryDataText#d0c3a112 text:string = dns.EntryData;
dns.entryDataNextResolver#13b13dc8 resolver:AccountAddress = dns.EntryData;
dns.entryDataSmcAddress#97197a42 smc_address:AccountAddress = dns.EntryData;
dns.entryDataAdnlAddress#bd98ba10 adnl_address:AdnlAddress = dns.EntryData;
dns.entryDataStorageAddress#97a0541c bag_id:int256 = dns.EntryData;

dns.entry#1e1b47a6 name:string category:int256 entry:dns.EntryData = dns.Entry;

dns.actionDeleteAll#3f9e909e = dns.Action;
// use category = 0 to delete all entries
dns.actionDelete#44077f51 name:string category:int256 = dns.Action;
dns.actionSet#ae0bb1c3 entry:dns.entry = dns.Action;

dns.resolved#7c970596 entries:vector<dns.entry> = dns.Resolved;


//
// Payment channel
//
pchan.promise#a20e945d signature:bytes promise_A:int64 promise_B:int64 channel_id:int64 = pchan.Promise;

pchan.actionInit#1a2bf68a inc_A:int64 inc_B:int64 min_A:int64 min_B:int64 = pchan.Action;
pchan.actionClose#639c4b16 extra_A:int64 extra_B:int64 promise:pchan.promise = pchan.Action;
pchan.actionTimeout#771e80f3 = pchan.Action;

//
// Restricted wallet initialization
//
rwallet.actionInit#2533bd6b config:rwallet.config = rwallet.Action;

//
// Actions
//

actionNoop#43b3ac9b = Action;
actionMsg#0eb67750 messages:vector<msg.message> allow_send_to_uninited:Bool = Action;
actionDns#47273021 actions:vector<dns.Action> = Action;
actionPchan#a72dc5e1 action:pchan.Action = Action;
actionRwallet#f90237c5 action:rwallet.actionInit = Action;
//actionMultisig actions:vector<multisig.order> = Action;

fees#63e9e6bc in_fwd_fee:int53 storage_fee:int53 gas_fee:int53 fwd_fee:int53 = Fees;
query.fees#603d17be source_fees:fees destination_fees:vector<fees> = query.Fees;
// query.emulationResult exit_code:int32 fees:fees = query.EmulationResult;
query.info#5689dc70 id:int53 valid_until:int53 body_hash:bytes body:bytes init_state:bytes = query.Info;

tvm.slice#20068ae7 bytes:bytes = tvm.Slice;
tvm.cell#e75ba3a1 bytes:bytes = tvm.Cell;
tvm.numberDecimal#45e296b3 number:string = tvm.Number;
tvm.tuple#aeb3ba63 elements:vector<tvm.StackEntry> = tvm.Tuple;
tvm.list#4bb78d08 elements:vector<tvm.StackEntry> = tvm.List;

tvm.stackEntrySlice#532d6b25 slice:tvm.slice = tvm.StackEntry;
tvm.stackEntryCell#4db16f20 cell:tvm.cell = tvm.StackEntry;
tvm.stackEntryNumber#50fb3dbe number:tvm.Number = tvm.StackEntry;
tvm.stackEntryTuple#f69e63dc tuple:tvm.Tuple = tvm.StackEntry;
tvm.stackEntryList#b9442d8b list:tvm.List = tvm.StackEntry;
tvm.stackEntryUnsupported#169541f2 = tvm.StackEntry;

smc.info#439b963c id:int53 = smc.Info;

smc.methodIdNumber#a423b9fc number:int32 = smc.MethodId;
smc.methodIdName#f127ff94 name:string = smc.MethodId;

smc.runResult#5444f3f3 gas_used:int53 stack:vector<tvm.StackEntry> exit_code:int32 = smc.RunResult;

smc.libraryEntry#a3d5d20c hash:int256 data:bytes = smc.LibraryEntry;
smc.libraryResult#0c27bbfe result:vector<smc.libraryEntry> = smc.LibraryResult;

smc.libraryQueryExt.one#6424eae3 hash:int256 = smc.LibraryQueryExt;
smc.libraryQueryExt.scanBoc#df54a4e5 boc:bytes max_libs:int32 = smc.LibraryQueryExt;
smc.libraryResultExt#b4deed72 dict_boc:bytes libs_ok:(vector int256) libs_not_found:(vector int256) = smc.LibraryResultExt;

updateSendLiteServerQuery#a34e95dc id:int64 data:bytes = Update;
updateSyncState#47c823de sync_state:SyncState = Update;

//@class LogStream @description Describes a stream to which tonlib internal log is written

//@description The log is written to stderr or an OS specific log
logStreamDefault#52e296bc = LogStream;

//@description The log is written to a file @path Path to the file to where the internal tonlib log will be written @max_file_size Maximum size of the file to where the internal tonlib log is written before the file will be auto-rotated
logStreamFile#8ff02a56 path:string max_file_size:int53 = LogStream;

//@description The log is written nowhere
logStreamEmpty#e233f1cc = LogStream;


//@description Contains a tonlib internal log verbosity level @verbosity_level Log verbosity level
logVerbosityLevel#676443ea verbosity_level:int32 = LogVerbosityLevel;

//@description Contains a list of available tonlib internal log tags @tags List of log tags
logTags#a056b3d7 tags:vector<string> = LogTags;

data#e747a971 bytes:secureBytes = Data;

liteServer.info#b57bfe73 now:int53 version:int32 capabilities:int64 = liteServer.Info;


blocks.masterchainInfo#31ca434b last:ton.BlockIdExt state_root_hash:bytes init:ton.BlockIdExt = blocks.MasterchainInfo;
blocks.shards#7b59a94a shards:vector<ton.BlockIdExt> = blocks.Shards;
blocks.accountTransactionId#cbf72284 account:bytes lt:int64 = blocks.AccountTransactionId;
blocks.shortTxId#1d15ef45 mode:# account:mode.0?bytes lt:mode.1?int64 hash:mode.2?bytes = liteServer.TransactionId;
blocks.transactions#8d6bf094 id:ton.blockIdExt req_count:int32 incomplete:Bool transactions:vector<blocks.shortTxId> = blocks.Transactions;
blocks.transactionsExt#4fc94e1d id:ton.blockIdExt req_count:int32 incomplete:Bool transactions:vector<raw.transaction> = blocks.TransactionsExt;
blocks.header#58298662 id:ton.blockIdExt global_id:int32 version:int32 flags:# after_merge:Bool after_split:Bool before_split:Bool want_merge:Bool want_split:Bool validator_list_hash_short:int32 catchain_seqno:int32 min_ref_mc_seqno:int32 is_key_block:Bool prev_key_block_seqno:int32 start_lt:int64 end_lt:int64 gen_utime:int53 vert_seqno:# prev_blocks:vector<ton.blockIdExt> = blocks.Header;
//blocks.shortData header:blocks.Header transactions:blocks.Header = blocks.BlockData;

blocks.signature#b71288c1 node_id_short:int256 signature:bytes = blocks.Signature;
blocks.blockSignatures#e801db9b id:ton.blockIdExt signatures:(vector blocks.signature) = blocks.BlockSignatures;
blocks.shardBlockLink#a6e01569 id:ton.blockIdExt proof:bytes = blocks.ShardBlockLink;
blocks.blockLinkBack#418c8035 to_key_block:Bool from:ton.blockIdExt to:ton.blockIdExt dest_proof:bytes proof:bytes state_proof:bytes = blocks.BlockLinkBack;
blocks.shardBlockProof#fbd65f53 from:ton.blockIdExt mc_id:ton.blockIdExt links:(vector blocks.shardBlockLink) mc_proof:(vector blocks.blockLinkBack) = blocks.ShardBlockProof;

configInfo#290055ff config:tvm.cell = ConfigInfo;

---functions---

init#c45c22b6 options:options = options.Info;
close#b933e17f = Ok;

options.setConfig#6f76ebc3 config:config = options.ConfigInfo;
options.validateConfig#eb51ba39 config:config = options.ConfigInfo;

createNewKey#910d8210 local_password:secureBytes mnemonic_password:secureBytes random_extra_seed:secureBytes = Key;
deleteKey#a1d948cd key:key = Ok;
deleteAllKeys#5fe3fb23 = Ok;
exportKey#9f4cd973 input_key:InputKey = ExportedKey;
exportPemKey#d9a8a3ba input_key:InputKey key_password:secureBytes = ExportedPemKey;
exportEncryptedKey#0d02097f input_key:InputKey key_password:secureBytes = ExportedEncryptedKey;
exportUnencryptedKey#da2bc740 input_key:InputKey = ExportedUnencryptedKey;
importKey#a0296119 local_password:secureBytes mnemonic_password:secureBytes exported_key:exportedKey = Key;
importPemKey#048d8d51 local_password:secureBytes key_password:secureBytes exported_key:exportedPemKey = Key;
importEncryptedKey#2724d3de local_password:secureBytes key_password:secureBytes exported_encrypted_key:exportedEncryptedKey = Key;
importUnencryptedKey#b9635915 local_password:secureBytes exported_unencrypted_key:exportedUnencryptedKey = Key;
changeLocalPassword#e81037bf input_key:InputKey new_local_password:secureBytes = Key;

encrypt#936f4b1c decrypted_data:secureBytes secret:secureBytes = Data;
decrypt#155685ae encrypted_data:secureBytes secret:secureBytes = Data;
kdf#9c96737d password:secureBytes salt:secureBytes iterations:int32 = Data;

unpackAccountAddress#d7528049 account_address:string = UnpackedAccountAddress;
packAccountAddress#ad3c39ec account_address:unpackedAccountAddress = AccountAddress;
getBip39Hints#8f5e5dea prefix:string = Bip39Hints;

//raw.init initial_account_state:raw.initialAccountState = Ok;
raw.getAccountState#b0daa932 account_address:accountAddress = raw.FullAccountState;
raw.getAccountStateByTransaction#2a91d0a3 account_address:accountAddress transaction_id:internal.transactionId = raw.FullAccountState;
raw.getTransactions#3d5ea31d private_key:InputKey account_address:accountAddress from_transaction_id:internal.transactionId = raw.Transactions;
raw.getTransactionsV2#de3f7ca6 private_key:InputKey account_address:accountAddress from_transaction_id:internal.transactionId count:# try_decode_messages:Bool = raw.Transactions;
raw.sendMessage#955780e0 body:bytes = Ok;
raw.sendMessageReturnHash#b6c76719 body:bytes = raw.ExtMessageInfo;
raw.createAndSendMessage#d1f8c9a5 destination:accountAddress initial_account_state:bytes data:bytes = Ok;
raw.createQuery#8d0c8aab destination:accountAddress init_code:bytes init_data:bytes body:bytes = query.Info;

sync#902edc92 = ton.BlockIdExt;

// revision = 0 -- use default revision
// revision = x (x > 0) -- use revision x
// revision = -1 -- use experimental (newest) revision. Only for debug purpose
//
// workchain_id = -1 or 0. -1 for masterchain, 0 for basechain
// NB: use wallet_id = default_wallet_id + workchain_id
getAccountAddress#1e8ba5c8 initial_account_state:InitialAccountState revision:int32 workchain_id:int32 = AccountAddress;
guessAccountRevision#6eb892a2 initial_account_state:InitialAccountState workchain_id:int32 = AccountRevisionList;

guessAccount#986d6c60 public_key:string rwallet_init_public_key:string = AccountRevisionList;

getAccountState#81daf446 account_address:accountAddress = FullAccountState;
getAccountStateByTransaction#2d1c0fce account_address:accountAddress transaction_id:internal.transactionId = FullAccountState;
getShardAccountCell#989e941b account_address:accountAddress = tvm.Cell;
getShardAccountCellByTransaction#1841e3c1 account_address:accountAddress transaction_id:internal.transactionId = tvm.Cell;
createQuery#f18b20c5 private_key:InputKey address:accountAddress timeout:int32 action:Action initial_account_state:InitialAccountState = query.Info;

getConfigParam#f20702f3 mode:# param:# = ConfigInfo;
getConfigAll#1ce13850 mode:# = ConfigInfo;

msg.decrypt#0d53cf09 input_key:InputKey data:msg.dataEncryptedArray = msg.DataDecryptedArray;
msg.decryptWithProof#8222c881 proof:bytes data:msg.dataEncrypted = msg.Data;

query.send#37261573 id:int53 = Ok;
query.forget#b7c2925f id:int53 = Ok;
query.estimateFees#c6f54e41 id:int53 ignore_chksig:Bool = query.Fees;
// query.emulate id:int53 ignore_chksig:Bool = query.EmulationResult;
query.getInfo#d05b22db id:int53 = query.Info;

smc.load#ca25d03f account_address:accountAddress = smc.Info;
smc.loadByTransaction#87a54b31 account_address:accountAddress transaction_id:internal.transactionId = smc.Info;
smc.forget#364d31e6 id:int53 = Ok;
smc.getCode#81e61b98 id:int53 = tvm.Cell;
smc.getData#e6835349 id:int53 = tvm.Cell;
smc.getState#f338a9eb id:int53 = tvm.Cell;
smc.runGetMethod#f0c905aa id:int53 method:smc.MethodId stack:vector<tvm.StackEntry> = smc.RunResult;

smc.getLibraries#3089ee15 library_list:(vector int256) = smc.LibraryResult;
smc.getLibrariesExt#37814932 list:(vector smc.LibraryQueryExt) = smc.LibraryResultExt;

dns.resolve#6ac69536 account_address:accountAddress name:string category:int256 ttl:int32 = dns.Resolved;

pchan.signPromise#6c245f1e input_key:InputKey promise:pchan.promise = pchan.Promise;
pchan.validatePromise#0f64c4e2 public_key:bytes promise:pchan.promise = Ok;

pchan.packPromise#cd3c0ac1 promise:pchan.promise = Data;
pchan.unpackPromise#b57ce4d3 data:secureBytes = pchan.Promise;


blocks.getMasterchainInfo#fd49d291 = blocks.MasterchainInfo;
blocks.getShards#7b8c042d id:ton.blockIdExt = blocks.Shards;
blocks.lookupBlock#548c5bb3 mode:int32 id:ton.blockId lt:int64 utime:int32 = ton.BlockIdExt;
blocks.getTransactions#ca95cd31 id:ton.blockIdExt mode:# count:# after:blocks.accountTransactionId = blocks.Transactions;
blocks.getTransactionsExt#a4f004ce id:ton.blockIdExt mode:# count:# after:blocks.accountTransactionId = blocks.TransactionsExt;
blocks.getBlockHeader#72262342 id:ton.blockIdExt = blocks.Header;
blocks.getMasterchainBlockSignatures#606025d4 seqno:int32 = blocks.BlockSignatures;
blocks.getShardBlockProof#19ed9ee7 id:ton.blockIdExt mode:# from:mode.0?ton.blockIdExt = blocks.ShardBlockProof;

onLiteServerQueryResult#7a92da5e id:int64 bytes:bytes = Ok;
onLiteServerQueryError#d79f46b3 id:int64 error:error = Ok;

withBlock#d0f762a5 id:ton.blockIdExt function:Function = Object;

runTests#8669354d dir:string = Ok;

liteServer.getInfo#558d5bee = liteServer.Info;

//@description Sets new log stream for internal logging of tonlib. This is an offline method. Can be called before authorization. Can be called synchronously @log_stream New log stream
setLogStream#aeaff791 log_stream:LogStream = Ok;

//@description Returns information about currently used log stream for internal logging of tonlib. This is an offline method. Can be called before authorization. Can be called synchronously
getLogStream#45984b5b = LogStream;

//@description Sets the verbosity level of the internal logging of tonlib. This is an offline method. Can be called before authorization. Can be called synchronously
//@new_verbosity_level New value of the verbosity level for logging. Value 0 corresponds to fatal errors, value 1 corresponds to errors, value 2 corresponds to warnings and debug warnings, value 3 corresponds to informational, value 4 corresponds to debug, value 5 corresponds to verbose debug, value greater than 5 and up to 1023 can be used to enable even more logging
setLogVerbosityLevel#edea07d2 new_verbosity_level:int32 = Ok;

//@description Returns current verbosity level of the internal logging of tonlib. This is an offline method. Can be called before authorization. Can be called synchronously
getLogVerbosityLevel#23689ae4 = LogVerbosityLevel;

//@description Returns list of available tonlib internal log tags, for example, ["actor", "binlog", "connections", "notifications", "proxy"]. This is an offline method. Can be called before authorization. Can be called synchronously
getLogTags#f0d569da = LogTags;

//@description Sets the verbosity level for a specified tonlib internal log tag. This is an offline method. Can be called before authorization. Can be called synchronously
//@tag Logging tag to change verbosity level @new_verbosity_level New verbosity level; 1-1024
setLogTagVerbosityLevel#8317d696 tag:string new_verbosity_level:int32 = Ok;

//@description Returns current verbosity level for a specified tonlib internal log tag. This is an offline method. Can be called before authorization. Can be called synchronously @tag Logging tag to change verbosity level
getLogTagVerbosityLevel#38af2d83 tag:string = LogVerbosityLevel;

//@description Adds a message to tonlib internal log. This is an offline method. Can be called before authorization. Can be called synchronously
//@verbosity_level Minimum verbosity level needed for the message to be logged, 0-1023 @text Text of a message to log
addLogMessage#5f36cfec verbosity_level:int32 text:string = Ok;
