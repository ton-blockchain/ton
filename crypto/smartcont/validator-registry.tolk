/*
    Validator registry is a contract that holds certain data for validators.
    Each validator can modify its own registry entry using a signed external message.
 */
import "@stdlib/gas-payments"
import "@stdlib/tvm-dicts"
import "@stdlib/tvm-lowlevel"

const VAL_REGISTRY_CONFIG_PARAM = 46;
const ELECTOR_ADDR_CONFIG_PARAM = 1;
const GAS_LIMIT = 68000000;

const ERR_PARSING_FAILED = 63;
const ERR_NOT_ENOUGH_GRAMS = 901;
const ERR_INVALID_ROLES = 902;
const ERR_NO_CONFIG = 903;
const ERR_DUPLICATE_VOTING = 904;
const ERR_BAD_NEW_CODE_HASH = 905;
const ERR_NEW_CODE_ALREADY_INSTALLED = 906;
const ERR_NO_GC_NEEDED = 907;
const ERR_REQUEST_EXPIRED = 908;
const ERR_VOTING_NOT_FOUND = 909;
const ERR_VOTING_EXPIRED = 910;
const ERR_WRONG_CREATED_LT = 911;
const ERR_WRONG_VALIDATOR_IDX = 912;
const ERR_ALREADY_VOTED = 913;
const ERR_WRONG_SIGNATURE = 914;
const ERR_MAX_NODES_EXCEEDED = 915;
const ERR_VAL_SET_UPDATE_NOT_REQUIRED = 916;
const ERR_UNKNOWN_ADNL_ID = 917;
const ERR_TOO_EARLY_REQUEST = 918;
const ERR_BAD_ADNL_NODE = 919;
const ERR_TOO_BIG_VALID_UNTIL = 920;

contract ValidatorRegistry {
    storage: Storage
}

struct (0x9bc722a9) Storage {
    nodes: map<uint256, NodeInfo>,
    votings: map<VotingKey, Voting>,
    votingsGc: map<VotingGcKey, ()>,
    extra: Cell<StorageExtra>,
    lastValidatorSetUpdateSeqno: uint32,
}

struct (0xc0c74297) StorageExtra {
    nodesCountByRole: map<uint5, uint32>,
    currentValidatorSets: Cell<AllValidatorSets>,
}

struct (0xca4882d5) AllValidatorSets {
    set32: cell?,
    set34: cell?,
    set36: cell?,
}

struct (0xac83c7be) NodeInfo {
    roles: uint32 = 0,
    isValidator: uint32 = 0,
    allowUpdateAdnlNodeAt: uint32 = 0,
    adnlNode: cell? = null, // contains serialized adnl.node tl
}

struct VotingKey {
    adnlId: uint256,
    rolesAdd: uint32,
    rolesRemove: uint32,
}

struct VotingGcKey {
    endsAt: uint32,
    adnlId: uint256,
    rolesAdd: uint32,
    rolesRemove: uint32,
}

struct (0xfb84df63) Voting {
    endsAt: uint32,
    createdLt: uint64,
    votedWeight: uint64,
    requiredWeight: uint64,
    votes: cell,
    validatorSet: cell,
}

struct (0x3601163f) RegistryConfig {
    addr: uint256,
    newCodeHash: uint256?,
    roles: map<uint5, RoleConfig>,
    votingTimeout: uint32,
    votingPrice: coins,
    nodeUpdatePeriod: uint32,
}

struct (0xe19f2d29) RoleConfig {
    thresholdAdd: uint16,
    thresholdRemove: uint16,
    maxNodes: uint32,
}

struct ValidatorSet {
    total: uint16,
    totalWeight: uint64,
    validators: map<uint16, slice>,
}

struct ValidatorDescr {
    publicKey: uint256,
    weight: uint64,
    adnlId: uint256?,
}

struct (0x3b02088d) MessageNewVoting {
    adnlId: uint256,
    rolesAdd: uint32,
    rolesRemove: uint32,
}

struct (0x34bdaa6c) MessageUpdateCode {
    newCode: cell,
}

struct (0x6065837c) MessageGcVotings {
    nonce: uint64,
}

struct (0x5cf70d63) MessageUpdateValidatorSet {
    nonce: uint64,
}

struct (0x17bbf84f) MessageSignedRequest {
    signature: bits512,
    request: Cell<RequestVote | RequestUpdateAdnlNode>,
}

struct (0xfcfffd3b) RequestVote {
    adnlId: uint256,
    rolesAdd: uint32,
    rolesRemove: uint32,
    validatorIdx: uint16,
    createdLt: uint64,
    validUntil: uint32,
}

struct (0xdbcf8cf8) RequestUpdateAdnlNode {
    adnlIdFull: uint256,
    validUntil: uint32,
    newAdnlNode: cell?
}

type ExternalMessage = MessageUpdateCode | MessageGcVotings | MessageUpdateValidatorSet | MessageSignedRequest;

fun ValidatorSet.unpackFromSlice(mutate s: slice) {
    // validators_ext#12 utime_since:uint32 utime_until:uint32
    //   total:(## 16) main:(## 16) { main <= total } { main >= 1 }
    //   total_weight:uint64 list:(HashmapE 16 ValidatorDescr) = ValidatorSet;
    assert (s.loadUint(8) == 0x12) throw ERR_PARSING_FAILED;
    s.skipBits(32 + 32);
    val total = s.loadUint(16);
    s.skipBits(16);
    val totalWeight = s.loadUint(64);
    val validators = s.loadDict();
    return ValidatorSet{
        total: total, totalWeight: totalWeight, validators: createMapFromLowLevelDict(validators),
    };
}

fun ValidatorDescr.unpackFromSlice(mutate s: slice) {
    // ed25519_pubkey#8e81278a pubkey:bits256 = SigPubKey;
    // validator#53 public_key:SigPubKey weight:uint64 = ValidatorDescr;
    // validator_addr#73 public_key:SigPubKey weight:uint64 adnl_addr:bits256 = ValidatorDescr;
    val tag = s.loadUint(8);
    assert (s.loadUint(32) == 0x8e81278a) throw ERR_PARSING_FAILED;
    val publicKey = s.loadUint(256);
    val weight = s.loadUint(64);
    var adnlId: uint256? = null;
    if (tag == 0x73) {
        adnlId = s.loadUint(256);
    } else {
        assert (tag == 0x53) throw ERR_PARSING_FAILED;
    }
    return ValidatorDescr{
        publicKey: publicKey, weight: weight, adnlId: adnlId,
    };
}

fun publicKeyToShortId(key: uint256): uint256 {
    return beginCell().storeUint(0xc6b41348, 32).storeUint(key, 256).endCell().beginParse().bitsHash();
}

fun ValidatorDescr.getAdnlId(self): uint256 {
    if (self.adnlId != null && self.adnlId != 0) {
        return self.adnlId;
    }
    return publicKeyToShortId(self.publicKey);
}

fun AllValidatorSets.get(self, i: int): cell? {
    if (i == 0) {
        return self.set32;
    }
    if (i == 1) {
        return self.set34;
    }
    if (i == 2) {
        return self.set36;
    }
    throw VmExitCode.IntegerOutOfRange;
}

fun AllValidatorSets.set(mutate self, i: int, v: cell?) {
    if (i == 0) {
        self.set32 = v;
    } else if (i == 1) {
        self.set34 = v;
    } else if (i == 2) {
        self.set36 = v;
    } else {
        throw VmExitCode.IntegerOutOfRange;
    }
}

fun getLastKeyBlockSeqno(): uint32
    asm "PREVKEYBLOCK 2 INDEX";

fun builder.storeZeroes(mutate self, n: int): self
    asm(self n) "STZEROES";

fun getRegistryConfigCell(): cell {
    var c = blockchain.configParam(VAL_REGISTRY_CONFIG_PARAM);
    assert (c != null) throw ERR_NO_CONFIG;
    return c;
}

fun getRegistryConfig(): RegistryConfig {
    return RegistryConfig.fromCell(getRegistryConfigCell());
}

fun getElectorAddress(): (int8, uint256) {
    var s = blockchain.configParam(ELECTOR_ADDR_CONFIG_PARAM)!.beginParse();
    return (-1, s.loadUint(256));
}

fun onInternalMessage(in: InMessage) {
    val msg = MessageNewVoting.fromSlice(in.body);
    val config = getRegistryConfig();
    assert (in.valueCoins >= config.votingPrice) throw ERR_NOT_ENOUGH_GRAMS;
    assert ((msg.rolesAdd & msg.rolesRemove) == 0) throw ERR_INVALID_ROLES;
    assert ((msg.rolesAdd | msg.rolesRemove) != 0) throw ERR_INVALID_ROLES;
    val votingKey = VotingKey{
        adnlId: msg.adnlId,
        rolesAdd: msg.rolesAdd,
        rolesRemove: msg.rolesRemove,
    };
    var storage = Storage.fromCell(contract.getData());
    assert (!storage.votings.exists(votingKey)) throw ERR_DUPLICATE_VOTING;
    var i = 0;
    var threshold = 0;
    while (i < 32) {
        val add = (msg.rolesAdd & (1 << i)) != 0;
        val remove = (msg.rolesRemove & (1 << i)) != 0;
        if (add || remove) {
            val f = config.roles.get(i);
            assert (f.isFound) throw ERR_INVALID_ROLES;
            val roleConfig = f.loadValue();
            threshold = max(threshold, add ? roleConfig.thresholdAdd : roleConfig.thresholdRemove);
        }
        i += 1;
    }

    val endsAt = blockchain.now() + config.votingTimeout;
    val validatorSetCell = blockchain.configParam(34)!;
    val validatorSet = ValidatorSet.fromCell(validatorSetCell);
    storage.votings.set(votingKey, Voting{
        endsAt: endsAt,
        createdLt: blockchain.logicalTime(),
        votedWeight: 0,
        requiredWeight: validatorSet.totalWeight * threshold / 10000,
        votes: beginCell().storeZeroes(validatorSet.total).endCell(),
        validatorSet: validatorSetCell,
    });
    storage.votingsGc.set(VotingGcKey{
        endsAt: endsAt,
        adnlId: msg.adnlId,
        rolesAdd: msg.rolesAdd,
        rolesRemove: msg.rolesRemove,
    }, ());

    createMessage({
        bounce: BounceMode.NoBounce,
        dest: getElectorAddress(),
        value: config.votingPrice,
    }).send(SEND_MODE_BOUNCE_ON_ACTION_FAIL);
    createMessage({
        bounce: BounceMode.NoBounce,
        dest: in.senderAddress,
        value: in.valueCoins - config.votingPrice,
    }).send(SEND_MODE_IGNORE_ERRORS);
    contract.setData(storage.toCell());
}

fun updateCode(newCode: cell) {
    // Don't parse the whole config, allowing changes in its tlb schema before updating code
    var configSlice = getRegistryConfigCell().beginParse();
    configSlice.skipBits(32 + 256);
    assert (configSlice.loadBool()) throw ERR_BAD_NEW_CODE_HASH;
    val newCodeHash = configSlice.loadUint(256);
    assert (newCodeHash == newCode.hash()) throw ERR_BAD_NEW_CODE_HASH;
    assert (contract.getCode().hash() != newCodeHash) throw ERR_NEW_CODE_ALREADY_INSTALLED;
    contract.setCodePostponed(newCode);
    setTvmRegisterC3(transformSliceToContinuation(newCode.beginParse()));
    acceptExternalMessage();
    afterUpdateCode();
}

fun gcVotings() {
    var storage = Storage.fromCell(contract.getData());
    val now = blockchain.now();
    var f = storage.votingsGc.findFirst();
    assert (f.isFound && f.getKey().endsAt <= now) throw ERR_NO_GC_NEEDED;
    acceptExternalMessage();
    while (f.isFound && f.getKey().endsAt <= now && getGasConsumedAtTheMoment() < GAS_LIMIT) {
        val key = f.getKey();
        storage.votings.delete(VotingKey{
            adnlId: key.adnlId,
            rolesAdd: key.rolesAdd,
            rolesRemove: key.rolesRemove,
        });
        storage.votingsGc.delete(key);
        f = storage.votingsGc.findFirst();
    }
    contract.setData(storage.toCell());
}

fun updateValidatorSetNodes(mutate storage: Storage, validatorSetCell: cell?, diff: int) {
    if (validatorSetCell == null) {
        return;
    }
    val validatorSet = ValidatorSet.fromCell(validatorSetCell);
    var f = validatorSet.validators.findFirst();
    while (f.isFound) {
        val adnlId = ValidatorDescr.fromSlice(f.loadValue()).getAdnlId();
        val f2 = storage.nodes.get(adnlId);
        var node = f2.isFound ? f2.loadValue() : NodeInfo{
        };
        node.isValidator += diff;
        if (node.roles != 0 || node.isValidator != 0) {
            storage.nodes.set(adnlId, node);
        } else {
            storage.nodes.delete(adnlId);
        }
        f = validatorSet.validators.iterateNext(f);
    }
}

fun updateValidatorSet() {
    var storage = Storage.fromCell(contract.getData());
    val lastKeyBlockSeqno = getLastKeyBlockSeqno();
    assert (storage.lastValidatorSetUpdateSeqno != lastKeyBlockSeqno) throw ERR_VAL_SET_UPDATE_NOT_REQUIRED;
    storage.lastValidatorSetUpdateSeqno = lastKeyBlockSeqno;
    contract.setData(storage.toCell());
    commitContractDataAndActions();
    acceptExternalMessage();

    var storageExtra = storage.extra.load();
    var prev = storageExtra.currentValidatorSets.load();
    val next = AllValidatorSets{
        set32: blockchain.configParam(32),
        set34: blockchain.configParam(34),
        set36: blockchain.configParam(36),
    };
    var i = 0;
    while (i < 3) {
        val set = next.get(i);
        if (set != null) {
            var j = 0;
            while (j < 3 && (prev.get(j) == null || prev.get(j)!.hash() != set.hash())) {
                j += 1;
            }
            if (j < 3) {
                prev.set(j, null);
            } else {
                updateValidatorSetNodes(mutate storage, set, 1);
            }
        }
        i += 1;
    }
    i = 0;
    while (i < 3) {
        updateValidatorSetNodes(mutate storage, prev.get(i), -1);
        i += 1;
    }

    storageExtra.currentValidatorSets = next.toCell();
    storage.extra = storageExtra.toCell();
    contract.setData(storage.toCell());
}

fun requestVote(signature: bits512, requestHash: uint256, request: RequestVote) {
    assert (blockchain.now() < request.validUntil) throw ERR_REQUEST_EXPIRED;
    var storage = Storage.fromCell(contract.getData());
    val key = VotingKey{
        adnlId: request.adnlId, rolesAdd: request.rolesAdd, rolesRemove: request.rolesRemove,
    };
    val f = storage.votings.get(key);
    assert (f.isFound) throw ERR_VOTING_NOT_FOUND;
    var voting = f.loadValue();
    assert (blockchain.now() < voting.endsAt) throw ERR_VOTING_EXPIRED;
    assert (request.createdLt == voting.createdLt) throw ERR_WRONG_CREATED_LT;

    var votes = voting.votes.beginParse();
    assert (request.validatorIdx < votes.remainingBitsCount()) throw ERR_WRONG_VALIDATOR_IDX;
    var newVotes = beginCell();
    newVotes.storeSlice(votes.loadBits(request.validatorIdx));
    assert (!votes.loadBool()) throw ERR_ALREADY_VOTED;
    newVotes.storeBool(true);
    newVotes.storeSlice(votes);

    val validatorSet = ValidatorSet.fromCell(voting.validatorSet);
    val validatorDescr = ValidatorDescr.fromSlice(validatorSet.validators.mustGet(request.validatorIdx));
    assert (isSignatureValid(requestHash, signature as slice, validatorDescr.publicKey)) throw ERR_WRONG_SIGNATURE;
    acceptExternalMessage();

    voting.votes = newVotes.endCell();
    voting.votedWeight += validatorDescr.weight;

    if (voting.votedWeight < voting.requiredWeight) {
        storage.votings.set(key, voting);
    } else {
        storage.votings.delete(key);
        storage.votingsGc.delete(VotingGcKey{
            endsAt: voting.endsAt, adnlId: key.adnlId, rolesAdd: key.rolesAdd, rolesRemove: key.rolesRemove,
        });
        contract.setData(storage.toCell());
        commitContractDataAndActions();
        val f2 = storage.nodes.get(key.adnlId);
        var node = f2.isFound ? f2.loadValue() : NodeInfo{
        };

        val config = getRegistryConfig();
        var newRoles = node.roles;
        newRoles |= key.rolesAdd;
        newRoles &= ~key.rolesRemove;
        var storageExtra = storage.extra.load();
        var i = 0;
        while (i < 32) {
            val diff = ((newRoles >> i) & 1) - ((node.roles >> i) & 1);
            if (diff != 0) {
                val f = storageExtra.nodesCountByRole.get(i);
                val count = (f.isFound ? f.loadValue() : 0 as uint32) + diff;
                if (diff > 0 && count > config.roles.mustGet(i).maxNodes) {
                    throw ERR_MAX_NODES_EXCEEDED;
                }
                if (count == 0) {
                    storageExtra.nodesCountByRole.delete(i);
                } else {
                    storageExtra.nodesCountByRole.set(i, count);
                }
            }
            i += 1;
        }

        storage.extra = storageExtra.toCell();
        node.roles = newRoles;
        if (node.roles != 0 || node.isValidator != 0) {
            storage.nodes.set(key.adnlId, node);
        } else {
            storage.nodes.delete(key.adnlId);
        }
    }

    contract.setData(storage.toCell());
}

fun requestUpdateAdnlNode(signature: bits512, requestHash: uint256, request: RequestUpdateAdnlNode) {
    val config = getRegistryConfig();
    assert (blockchain.now() < request.validUntil) throw ERR_REQUEST_EXPIRED;
    assert (request.validUntil < blockchain.now() + config.nodeUpdatePeriod) throw ERR_TOO_BIG_VALID_UNTIL;
    var storage = Storage.fromCell(contract.getData());
    val adnlId = publicKeyToShortId(request.adnlIdFull);
    var node = storage.nodes.mustGet(adnlId, ERR_UNKNOWN_ADNL_ID);
    assert (blockchain.now() >= node.allowUpdateAdnlNodeAt) throw ERR_TOO_EARLY_REQUEST;
    if (request.newAdnlNode != null) {
        val (bits, refs) = request.newAdnlNode.beginParse().remainingBitsAndRefsCount();
        assert (bits % 8 == 0 && refs == 0) throw ERR_BAD_ADNL_NODE;
    }
    assert (isSignatureValid(requestHash, signature as slice, request.adnlIdFull)) throw ERR_WRONG_SIGNATURE;
    acceptExternalMessage();

    node.adnlNode = request.newAdnlNode;
    node.allowUpdateAdnlNodeAt = blockchain.now() + config.nodeUpdatePeriod;
    storage.nodes.set(adnlId, node);
    contract.setData(storage.toCell());
}

fun onExternalMessage(inMsg: slice) {
    val msg = ExternalMessage.fromSlice(inMsg);
    match (msg) {
        MessageUpdateCode => {
            updateCode(msg.newCode);
        }
        MessageGcVotings => {
            gcVotings();
        }
        MessageUpdateValidatorSet => {
            updateValidatorSet();
        }
        MessageSignedRequest => {
            val request = msg.request.load();
            match (request) {
                RequestVote => {
                    requestVote(msg.signature, msg.request.hash(), request);
                }
                RequestUpdateAdnlNode => {
                    requestUpdateAdnlNode(msg.signature, msg.request.hash(), request);
                }
            }
        }
    }
}

get fun afterUpdateCode() {
}