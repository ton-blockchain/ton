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

contract ValidatorRegistry {
    storage: Storage
}

struct (0x9bc722a9) Storage {
    nodes: map<uint256, NodeInfo>,
    votings: map<VotingKey, Voting>,
    votingsGc: map<VotingGcKey, ()>,
}

struct (0xac83c7be) NodeInfo {
    roles: uint32 = 0,
    isValidator: uint8 = 0,
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
}

struct (0xe19f2d29) RoleConfig {
    thresholdAdd: uint16,
    thresholdRemove: uint16,
    maxNodes: uint32,
}

struct ValidatorSet {
    total: uint16,
    totalWeight: uint64,
    validators: dict,
}

struct ValidatorDescr {
    publicKey: uint256,
    weight: uint64,
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

struct (0x17bbf84f) MessageSignedRequest {
    signature: bits512,
    request: Cell<RequestVote>,
}

struct (0xfcfffd3b) RequestVote {
    adnlId: uint256,
    rolesAdd: uint32,
    rolesRemove: uint32,
    validatorIdx: uint16,
    createdLt: uint64,
    validUntil: uint32,
}

type ExternalMessage = MessageUpdateCode | MessageGcVotings | MessageSignedRequest;

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
        total: total, totalWeight: totalWeight, validators: validators,
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
    if (tag == 0x73) {
        s.skipBits(256);
    } else {
        assert (tag == 0x53) throw ERR_PARSING_FAILED;
    }
    return ValidatorDescr{
        publicKey: publicKey, weight: weight
    };
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
    val validatorDescr = ValidatorDescr.fromSlice(validatorSet.validators.uDictGet(16, request.validatorIdx).0!);
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
        val f2 = storage.nodes.get(key.adnlId);
        var node = f2.isFound ? f2.loadValue() : NodeInfo{
        };
        node.roles |= key.rolesAdd;
        node.roles &= ~key.rolesRemove;
        if (node.roles != 0 || node.isValidator != 0) {
            storage.nodes.set(key.adnlId, node);
        } else {
            storage.nodes.delete(key.adnlId);
        }
    }

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
        MessageSignedRequest => {
            val request = msg.request.load();
            requestVote(msg.signature, msg.request.hash(), request);
        }
    }
}

get fun afterUpdateCode() {
}