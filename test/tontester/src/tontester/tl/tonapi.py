import typing

import pydantic

type Int128 = typing.Annotated[pydantic.Base64Bytes, pydantic.Field(min_length=16, max_length=16)]
type Int256 = typing.Annotated[pydantic.Base64Bytes, pydantic.Field(min_length=32, max_length=32)]
type Option[T] = T | None


# ===== Hashable =====
class hashable_bool(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1c\x44\x61\xcf"
    tl_type: typing.Literal["hashable.bool"] = pydantic.Field(
        alias="@type", default="hashable.bool"
    )
    value: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_int32(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x56\x93\xb5\xd3"
    tl_type: typing.Literal["hashable.int32"] = pydantic.Field(
        alias="@type", default="hashable.int32"
    )
    value: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_int64(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x42\x8e\xda\xe7"
    tl_type: typing.Literal["hashable.int64"] = pydantic.Field(
        alias="@type", default="hashable.int64"
    )
    value: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_int256(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcf\x13\x23\x3a"
    tl_type: typing.Literal["hashable.int256"] = pydantic.Field(
        alias="@type", default="hashable.int256"
    )
    value: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_bytes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x12\xde\x13\x07"
    tl_type: typing.Literal["hashable.bytes"] = pydantic.Field(
        alias="@type", default="hashable.bytes"
    )
    value: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_pair(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x95\x68\xe5\xc7"
    tl_type: typing.Literal["hashable.pair"] = pydantic.Field(
        alias="@type", default="hashable.pair"
    )
    left: int = 0
    right: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_vector(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6d\xc3\x34\xdf"
    tl_type: typing.Literal["hashable.vector"] = pydantic.Field(
        alias="@type", default="hashable.vector"
    )
    value: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_validatorSessionOldRound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa9\x67\x8b\x47"
    tl_type: typing.Literal["hashable.validatorSessionOldRound"] = pydantic.Field(
        alias="@type", default="hashable.validatorSessionOldRound"
    )
    seqno: int = 0
    block: int = 0
    signatures: int = 0
    approve_signatures: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_validatorSessionRoundAttempt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xad\xff\x11\x4c"
    tl_type: typing.Literal["hashable.validatorSessionRoundAttempt"] = pydantic.Field(
        alias="@type", default="hashable.validatorSessionRoundAttempt"
    )
    seqno: int = 0
    votes: int = 0
    precommitted: int = 0
    vote_for_inited: int = 0
    vote_for: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_validatorSessionRound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe3\x4f\x77\x35"
    tl_type: typing.Literal["hashable.validatorSessionRound"] = pydantic.Field(
        alias="@type", default="hashable.validatorSessionRound"
    )
    locked_round: int = 0
    locked_block: int = 0
    seqno: int = 0
    precommitted: bool = False
    first_attempt: int = 0
    approved_blocks: int = 0
    signatures: int = 0
    attempts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_blockSignature(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa2\x92\xe1\x37"
    tl_type: typing.Literal["hashable.blockSignature"] = pydantic.Field(
        alias="@type", default="hashable.blockSignature"
    )
    signature: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_sentBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2b\x95\xb9\xbd"
    tl_type: typing.Literal["hashable.sentBlock"] = pydantic.Field(
        alias="@type", default="hashable.sentBlock"
    )
    src: int = 0
    root_hash: int = 0
    file_hash: int = 0
    collated_data_file_hash: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_sentBlockEmpty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xaf\x46\xf2\x9e"
    tl_type: typing.Literal["hashable.sentBlockEmpty"] = pydantic.Field(
        alias="@type", default="hashable.sentBlockEmpty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_vote(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc5\x2b\xbf\xae"
    tl_type: typing.Literal["hashable.vote"] = pydantic.Field(
        alias="@type", default="hashable.vote"
    )
    block: int = 0
    node: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_blockCandidate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0d\xb1\xa9\x0b"
    tl_type: typing.Literal["hashable.blockCandidate"] = pydantic.Field(
        alias="@type", default="hashable.blockCandidate"
    )
    block: int = 0
    approved: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_blockVoteCandidate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe5\x6f\x0d\xcf"
    tl_type: typing.Literal["hashable.blockVoteCandidate"] = pydantic.Field(
        alias="@type", default="hashable.blockVoteCandidate"
    )
    block: int = 0
    approved: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_blockCandidateAttempt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0b\x7d\x5c\x3f"
    tl_type: typing.Literal["hashable.blockCandidateAttempt"] = pydantic.Field(
        alias="@type", default="hashable.blockCandidateAttempt"
    )
    block: int = 0
    votes: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_cntVector(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x38\x6f\x28\x0b"
    tl_type: typing.Literal["hashable.cntVector"] = pydantic.Field(
        alias="@type", default="hashable.cntVector"
    )
    data: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_cntSortedVector(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x59\x46\x96\x7b"
    tl_type: typing.Literal["hashable.cntSortedVector"] = pydantic.Field(
        alias="@type", default="hashable.cntSortedVector"
    )
    data: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class hashable_validatorSession(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd5\x63\x12\x68"
    tl_type: typing.Literal["hashable.validatorSession"] = pydantic.Field(
        alias="@type", default="hashable.validatorSession"
    )
    ts: int = 0
    old_rounds: int = 0
    cur_round: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Hashable = typing.Annotated[
    hashable_bool
    | hashable_int32
    | hashable_int64
    | hashable_int256
    | hashable_bytes
    | hashable_pair
    | hashable_vector
    | hashable_validatorSessionOldRound
    | hashable_validatorSessionRoundAttempt
    | hashable_validatorSessionRound
    | hashable_blockSignature
    | hashable_sentBlock
    | hashable_sentBlockEmpty
    | hashable_vote
    | hashable_blockCandidate
    | hashable_blockVoteCandidate
    | hashable_blockCandidateAttempt
    | hashable_cntVector
    | hashable_cntSortedVector
    | hashable_validatorSession,
    pydantic.Field(discriminator="tl_type"),
]
Hashable_Model = pydantic.RootModel[Hashable]


# ===== ImportedMsgQueueLimits =====
class tonNode_importedMsgQueueLimits(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0b\xe3\xcd\xf7"
    tl_type: typing.Literal["tonNode.importedMsgQueueLimits"] = pydantic.Field(
        alias="@type", default="tonNode.importedMsgQueueLimits"
    )
    max_bytes: int = 0
    max_msgs: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ImportedMsgQueueLimits = tonNode_importedMsgQueueLimits
ImportedMsgQueueLimits_Model = pydantic.RootModel[ImportedMsgQueueLimits]


# ===== Ok =====
class storage_ok(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x05\x1c\x2b\xc3"
    tl_type: typing.Literal["storage.ok"] = pydantic.Field(alias="@type", default="storage.ok")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Ok = storage_ok
Ok_Model = pydantic.RootModel[Ok]


# ===== PrivateKey =====
class pk_unenc(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x30\x9b\xdb\xb1"
    tl_type: typing.Literal["pk.unenc"] = pydantic.Field(alias="@type", default="pk.unenc")
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pk_ed25519(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x17\x23\x68\x49"
    tl_type: typing.Literal["pk.ed25519"] = pydantic.Field(alias="@type", default="pk.ed25519")
    key: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pk_aes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x37\x51\xe8\xa5"
    tl_type: typing.Literal["pk.aes"] = pydantic.Field(alias="@type", default="pk.aes")
    key: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pk_overlay(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5b\xf6\xa5\x37"
    tl_type: typing.Literal["pk.overlay"] = pydantic.Field(alias="@type", default="pk.overlay")
    name: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type PrivateKey = typing.Annotated[
    pk_unenc | pk_ed25519 | pk_aes | pk_overlay, pydantic.Field(discriminator="tl_type")
]
PrivateKey_Model = pydantic.RootModel[PrivateKey]


# ===== PublicKey =====
class pub_unenc(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0a\x45\x1f\xb6"
    tl_type: typing.Literal["pub.unenc"] = pydantic.Field(alias="@type", default="pub.unenc")
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pub_ed25519(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc6\xb4\x13\x48"
    tl_type: typing.Literal["pub.ed25519"] = pydantic.Field(alias="@type", default="pub.ed25519")
    key: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pub_aes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd4\xad\xbc\x2d"
    tl_type: typing.Literal["pub.aes"] = pydantic.Field(alias="@type", default="pub.aes")
    key: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pub_overlay(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcb\x45\xba\x34"
    tl_type: typing.Literal["pub.overlay"] = pydantic.Field(alias="@type", default="pub.overlay")
    name: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type PublicKey = typing.Annotated[
    pub_unenc | pub_ed25519 | pub_aes | pub_overlay, pydantic.Field(discriminator="tl_type")
]
PublicKey_Model = pydantic.RootModel[PublicKey]


# ===== adnl.Address =====
class adnl_address_udp(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe7\xa6\x0d\x67"
    tl_type: typing.Literal["adnl.address.udp"] = pydantic.Field(
        alias="@type", default="adnl.address.udp"
    )
    ip: int = 0
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_address_udp6(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfa\x63\x1d\xe3"
    tl_type: typing.Literal["adnl.address.udp6"] = pydantic.Field(
        alias="@type", default="adnl.address.udp6"
    )
    ip: Int128 = b"\x00" * 16
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_address_tunnel(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xeb\x02\x2b\x09"
    tl_type: typing.Literal["adnl.address.tunnel"] = pydantic.Field(
        alias="@type", default="adnl.address.tunnel"
    )
    to: Int256 = b"\x00" * 32
    pubkey: Option["PublicKey"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_address_reverse(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x86\x52\x79\x27"
    tl_type: typing.Literal["adnl.address.reverse"] = pydantic.Field(
        alias="@type", default="adnl.address.reverse"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Address = typing.Annotated[
    adnl_address_udp | adnl_address_udp6 | adnl_address_tunnel | adnl_address_reverse,
    pydantic.Field(discriminator="tl_type"),
]
adnl_Address_Model = pydantic.RootModel[adnl_Address]


# ===== adnl.AddressList =====
class adnl_addressList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x58\xe6\x27\x22"
    tl_type: typing.Literal["adnl.addressList"] = pydantic.Field(
        alias="@type", default="adnl.addressList"
    )
    addrs: list["adnl_Address"] = []
    version: int = 0
    reinit_date: int = 0
    priority: int = 0
    expire_at: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_AddressList = adnl_addressList
adnl_AddressList_Model = pydantic.RootModel[adnl_AddressList]


# ===== adnl.Message =====
class adnl_message_createChannel(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbb\xc3\x73\xe6"
    tl_type: typing.Literal["adnl.message.createChannel"] = pydantic.Field(
        alias="@type", default="adnl.message.createChannel"
    )
    key: Int256 = b"\x00" * 32
    date: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_confirmChannel(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x69\x1d\xdd\x60"
    tl_type: typing.Literal["adnl.message.confirmChannel"] = pydantic.Field(
        alias="@type", default="adnl.message.confirmChannel"
    )
    key: Int256 = b"\x00" * 32
    peer_key: Int256 = b"\x00" * 32
    date: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_custom(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf5\x18\x48\x20"
    tl_type: typing.Literal["adnl.message.custom"] = pydantic.Field(
        alias="@type", default="adnl.message.custom"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_nop(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xda\xdf\xf8\x17"
    tl_type: typing.Literal["adnl.message.nop"] = pydantic.Field(
        alias="@type", default="adnl.message.nop"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_reinit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x20\x05\xc2\x10"
    tl_type: typing.Literal["adnl.message.reinit"] = pydantic.Field(
        alias="@type", default="adnl.message.reinit"
    )
    date: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_query(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7a\xf9\x8b\xb4"
    tl_type: typing.Literal["adnl.message.query"] = pydantic.Field(
        alias="@type", default="adnl.message.query"
    )
    query_id: Int256 = b"\x00" * 32
    query: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_answer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x16\x84\xac\x0f"
    tl_type: typing.Literal["adnl.message.answer"] = pydantic.Field(
        alias="@type", default="adnl.message.answer"
    )
    query_id: Int256 = b"\x00" * 32
    answer: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_message_part(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x39\x2d\x45\xfd"
    tl_type: typing.Literal["adnl.message.part"] = pydantic.Field(
        alias="@type", default="adnl.message.part"
    )
    hash: Int256 = b"\x00" * 32
    total_size: int = 0
    offset: int = 0
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Message = typing.Annotated[
    adnl_message_createChannel
    | adnl_message_confirmChannel
    | adnl_message_custom
    | adnl_message_nop
    | adnl_message_reinit
    | adnl_message_query
    | adnl_message_answer
    | adnl_message_part,
    pydantic.Field(discriminator="tl_type"),
]
adnl_Message_Model = pydantic.RootModel[adnl_Message]


# ===== adnl.Node =====
class adnl_node(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x85\x12\x56\x6b"
    tl_type: typing.Literal["adnl.node"] = pydantic.Field(alias="@type", default="adnl.node")
    id: Option["PublicKey"] = None
    addr_list: Option["adnl_AddressList"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Node = adnl_node
adnl_Node_Model = pydantic.RootModel[adnl_Node]


# ===== adnl.Nodes =====
class adnl_nodes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x56\xdb\x09\xa2"
    tl_type: typing.Literal["adnl.nodes"] = pydantic.Field(alias="@type", default="adnl.nodes")
    nodes: list["adnl_Node"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Nodes = adnl_nodes
adnl_Nodes_Model = pydantic.RootModel[adnl_Nodes]
type adnl_PacketContents = None  # unsupported


# ===== adnl.Pong =====
class adnl_pong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0e\x7c\x74\x20"
    tl_type: typing.Literal["adnl.pong"] = pydantic.Field(alias="@type", default="adnl.pong")
    value: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Pong = adnl_pong
adnl_Pong_Model = pydantic.RootModel[adnl_Pong]


# ===== adnl.Proxy =====
class adnl_proxy_none(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7b\x48\x32\x35"
    tl_type: typing.Literal["adnl.proxy.none"] = pydantic.Field(
        alias="@type", default="adnl.proxy.none"
    )
    id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_proxy_fast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb5\x45\x8b\x3a"
    tl_type: typing.Literal["adnl.proxy.fast"] = pydantic.Field(
        alias="@type", default="adnl.proxy.fast"
    )
    id: Int256 = b"\x00" * 32
    shared_secret: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Proxy = typing.Annotated[
    adnl_proxy_none | adnl_proxy_fast, pydantic.Field(discriminator="tl_type")
]
adnl_Proxy_Model = pydantic.RootModel[adnl_Proxy]


# ===== adnl.ProxyControlPacket =====
class adnl_proxyControlPacketPing(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4b\xe4\x96\x37"
    tl_type: typing.Literal["adnl.proxyControlPacketPing"] = pydantic.Field(
        alias="@type", default="adnl.proxyControlPacketPing"
    )
    id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_proxyControlPacketPong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfc\xdb\xd1\x4b"
    tl_type: typing.Literal["adnl.proxyControlPacketPong"] = pydantic.Field(
        alias="@type", default="adnl.proxyControlPacketPong"
    )
    id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class adnl_proxyControlPacketRegister(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3f\xb2\x09\xc3"
    tl_type: typing.Literal["adnl.proxyControlPacketRegister"] = pydantic.Field(
        alias="@type", default="adnl.proxyControlPacketRegister"
    )
    ip: int = 0
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_ProxyControlPacket = typing.Annotated[
    adnl_proxyControlPacketPing | adnl_proxyControlPacketPong | adnl_proxyControlPacketRegister,
    pydantic.Field(discriminator="tl_type"),
]
adnl_ProxyControlPacket_Model = pydantic.RootModel[adnl_ProxyControlPacket]
type adnl_ProxyPacketHeader = None  # unsupported


# ===== adnl.ProxyTo =====
class adnl_proxyToFastHash(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5e\xf8\xbd\xdd"
    tl_type: typing.Literal["adnl.proxyToFastHash"] = pydantic.Field(
        alias="@type", default="adnl.proxyToFastHash"
    )
    ip: int = 0
    port: int = 0
    date: int = 0
    data_hash: Int256 = b"\x00" * 32
    shared_secret: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_ProxyTo = adnl_proxyToFastHash
adnl_ProxyTo_Model = pydantic.RootModel[adnl_ProxyTo]


# ===== adnl.ProxyToSign =====
class adnl_proxyToFast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd6\x21\xee\xb4"
    tl_type: typing.Literal["adnl.proxyToFast"] = pydantic.Field(
        alias="@type", default="adnl.proxyToFast"
    )
    ip: int = 0
    port: int = 0
    date: int = 0
    signature: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_ProxyToSign = adnl_proxyToFast
adnl_ProxyToSign_Model = pydantic.RootModel[adnl_ProxyToSign]


# ===== adnl.Stats =====
class adnl_stats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9d\x50\xaf\x61"
    tl_type: typing.Literal["adnl.stats"] = pydantic.Field(alias="@type", default="adnl.stats")
    timestamp: float = 0
    local_ids: list["adnl_stats_LocalId"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_Stats = adnl_stats
adnl_Stats_Model = pydantic.RootModel[adnl_Stats]
type adnl_TunnelPacketContents = None  # unsupported


# ===== adnl.config.Global =====
class adnl_config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd0\x80\x6f\xbe"
    tl_type: typing.Literal["adnl.config.global"] = pydantic.Field(
        alias="@type", default="adnl.config.global"
    )
    static_nodes: Option["adnl_Nodes"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_config_Global = adnl_config_global
adnl_config_Global_Model = pydantic.RootModel[adnl_config_Global]


# ===== adnl.db.Key =====
class adnl_db_node_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2e\xe4\xa3\xc5"
    tl_type: typing.Literal["adnl.db.node.key"] = pydantic.Field(
        alias="@type", default="adnl.db.node.key"
    )
    local_id: Int256 = b"\x00" * 32
    peer_id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_db_Key = adnl_db_node_key
adnl_db_Key_Model = pydantic.RootModel[adnl_db_Key]


# ===== adnl.db.node.Value =====
class adnl_db_node_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x07\x27\x5d\x54"
    tl_type: typing.Literal["adnl.db.node.value"] = pydantic.Field(
        alias="@type", default="adnl.db.node.value"
    )
    date: int = 0
    id: Option["PublicKey"] = None
    addr_list: Option["adnl_AddressList"] = None
    priority_addr_list: Option["adnl_AddressList"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_db_node_Value = adnl_db_node_value
adnl_db_node_Value_Model = pydantic.RootModel[adnl_db_node_Value]


# ===== adnl.id.Short =====
class adnl_id_short(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\x65\x3f\x3e"
    tl_type: typing.Literal["adnl.id.short"] = pydantic.Field(
        alias="@type", default="adnl.id.short"
    )
    id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_id_Short = adnl_id_short
adnl_id_Short_Model = pydantic.RootModel[adnl_id_Short]


# ===== adnl.stats.IpPackets =====
class adnl_stats_ipPackets(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9a\x4b\xa6\xa2"
    tl_type: typing.Literal["adnl.stats.ipPackets"] = pydantic.Field(
        alias="@type", default="adnl.stats.ipPackets"
    )
    ip_str: str = ""
    packets: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_stats_IpPackets = adnl_stats_ipPackets
adnl_stats_IpPackets_Model = pydantic.RootModel[adnl_stats_IpPackets]


# ===== adnl.stats.LocalId =====
class adnl_stats_localId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfe\x55\xf2\x1c"
    tl_type: typing.Literal["adnl.stats.localId"] = pydantic.Field(
        alias="@type", default="adnl.stats.localId"
    )
    short_id: Int256 = b"\x00" * 32
    current_decrypt: list["adnl_stats_IpPackets"] = []
    packets_recent: Option["adnl_stats_LocalIdPackets"] = None
    packets_total: Option["adnl_stats_LocalIdPackets"] = None
    peers: list["adnl_stats_PeerPair"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_stats_LocalId = adnl_stats_localId
adnl_stats_LocalId_Model = pydantic.RootModel[adnl_stats_LocalId]


# ===== adnl.stats.LocalIdPackets =====
class adnl_stats_localIdPackets(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x76\x37\x33\xf5"
    tl_type: typing.Literal["adnl.stats.localIdPackets"] = pydantic.Field(
        alias="@type", default="adnl.stats.localIdPackets"
    )
    ts_start: float = 0
    ts_end: float = 0
    decrypted_packets: list["adnl_stats_IpPackets"] = []
    dropped_packets: list["adnl_stats_IpPackets"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_stats_LocalIdPackets = adnl_stats_localIdPackets
adnl_stats_LocalIdPackets_Model = pydantic.RootModel[adnl_stats_LocalIdPackets]


# ===== adnl.stats.Packets =====
class adnl_stats_packets(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4b\x6d\x0b\x9d"
    tl_type: typing.Literal["adnl.stats.packets"] = pydantic.Field(
        alias="@type", default="adnl.stats.packets"
    )
    ts_start: float = 0
    ts_end: float = 0
    in_packets: int = 0
    in_bytes: int = 0
    in_packets_channel: int = 0
    in_bytes_channel: int = 0
    out_packets: int = 0
    out_bytes: int = 0
    out_packets_channel: int = 0
    out_bytes_channel: int = 0
    out_expired_messages: int = 0
    out_expired_bytes: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_stats_Packets = adnl_stats_packets
adnl_stats_Packets_Model = pydantic.RootModel[adnl_stats_Packets]


# ===== adnl.stats.PeerPair =====
class adnl_stats_peerPair(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x44\xdb\x8c\xa9"
    tl_type: typing.Literal["adnl.stats.peerPair"] = pydantic.Field(
        alias="@type", default="adnl.stats.peerPair"
    )
    local_id: Int256 = b"\x00" * 32
    peer_id: Int256 = b"\x00" * 32
    ip_str: str = ""
    packets_recent: Option["adnl_stats_Packets"] = None
    packets_total: Option["adnl_stats_Packets"] = None
    last_out_packet_ts: float = 0
    last_in_packet_ts: float = 0
    connection_ready: bool = False
    channel_status: int = 0
    try_reinit_at: float = 0
    out_queue_messages: int = 0
    out_queue_bytes: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type adnl_stats_PeerPair = adnl_stats_peerPair
adnl_stats_PeerPair_Model = pydantic.RootModel[adnl_stats_PeerPair]


# ===== catchain.Block =====
class catchain_block(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x74\x41\x55\xd6"
    tl_type: typing.Literal["catchain.block"] = pydantic.Field(
        alias="@type", default="catchain.block"
    )
    incarnation: Int256 = b"\x00" * 32
    src: int = 0
    height: int = 0
    data: Option["catchain_block_Data"] = None
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_Block = catchain_block
catchain_Block_Model = pydantic.RootModel[catchain_Block]


# ===== catchain.BlockResult =====
class catchain_blockNotFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\x08\x11\xb6"
    tl_type: typing.Literal["catchain.blockNotFound"] = pydantic.Field(
        alias="@type", default="catchain.blockNotFound"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class catchain_blockResult(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x47\x30\x2a\x9d"
    tl_type: typing.Literal["catchain.blockResult"] = pydantic.Field(
        alias="@type", default="catchain.blockResult"
    )
    block: Option["catchain_Block"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_BlockResult = typing.Annotated[
    catchain_blockNotFound | catchain_blockResult, pydantic.Field(discriminator="tl_type")
]
catchain_BlockResult_Model = pydantic.RootModel[catchain_BlockResult]


# ===== catchain.Blocks =====
class catchain_blocks(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc1\xd1\xec\x50"
    tl_type: typing.Literal["catchain.blocks"] = pydantic.Field(
        alias="@type", default="catchain.blocks"
    )
    blocks: list["catchain_Block"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_Blocks = catchain_blocks
catchain_Blocks_Model = pydantic.RootModel[catchain_Blocks]


# ===== catchain.Difference =====
class catchain_difference(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xca\xd1\x15\x14"
    tl_type: typing.Literal["catchain.difference"] = pydantic.Field(
        alias="@type", default="catchain.difference"
    )
    sent_upto: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class catchain_differenceFork(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6f\xc0\x27\x49"
    tl_type: typing.Literal["catchain.differenceFork"] = pydantic.Field(
        alias="@type", default="catchain.differenceFork"
    )
    left: Option["catchain_block_Dep"] = None
    right: Option["catchain_block_Dep"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_Difference = typing.Annotated[
    catchain_difference | catchain_differenceFork, pydantic.Field(discriminator="tl_type")
]
catchain_Difference_Model = pydantic.RootModel[catchain_Difference]


# ===== catchain.FirstBlock =====
class catchain_firstblock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfb\x04\xc9\x10"
    tl_type: typing.Literal["catchain.firstblock"] = pydantic.Field(
        alias="@type", default="catchain.firstblock"
    )
    unique_hash: Int256 = b"\x00" * 32
    nodes: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_FirstBlock = catchain_firstblock
catchain_FirstBlock_Model = pydantic.RootModel[catchain_FirstBlock]


# ===== catchain.Update =====
class catchain_blockUpdate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc4\x58\x67\x23"
    tl_type: typing.Literal["catchain.blockUpdate"] = pydantic.Field(
        alias="@type", default="catchain.blockUpdate"
    )
    block: Option["catchain_Block"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_Update = catchain_blockUpdate
catchain_Update_Model = pydantic.RootModel[catchain_Update]


# ===== catchain.block.Data =====
class catchain_block_data(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x20\xa6\xac\xf8"
    tl_type: typing.Literal["catchain.block.data"] = pydantic.Field(
        alias="@type", default="catchain.block.data"
    )
    prev: Option["catchain_block_Dep"] = None
    deps: list["catchain_block_Dep"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_block_Data = catchain_block_data
catchain_block_Data_Model = pydantic.RootModel[catchain_block_Data]


# ===== catchain.block.Dep =====
class catchain_block_dep(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\xd1\x1a\x5a"
    tl_type: typing.Literal["catchain.block.dep"] = pydantic.Field(
        alias="@type", default="catchain.block.dep"
    )
    src: int = 0
    height: int = 0
    data_hash: Int256 = b"\x00" * 32
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_block_Dep = catchain_block_dep
catchain_block_Dep_Model = pydantic.RootModel[catchain_block_Dep]


# ===== catchain.block.Id =====
class catchain_block_id(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xba\x98\xfe\x24"
    tl_type: typing.Literal["catchain.block.id"] = pydantic.Field(
        alias="@type", default="catchain.block.id"
    )
    incarnation: Int256 = b"\x00" * 32
    src: Int256 = b"\x00" * 32
    height: int = 0
    data_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_block_Id = catchain_block_id
catchain_block_Id_Model = pydantic.RootModel[catchain_block_Id]


# ===== catchain.block.inner.Data =====
class catchain_block_data_badBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x56\x5a\x02\xb6"
    tl_type: typing.Literal["catchain.block.data.badBlock"] = pydantic.Field(
        alias="@type", default="catchain.block.data.badBlock"
    )
    block: Option["catchain_Block"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class catchain_block_data_fork(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x52\x3a\x7a\x64"
    tl_type: typing.Literal["catchain.block.data.fork"] = pydantic.Field(
        alias="@type", default="catchain.block.data.fork"
    )
    left: Option["catchain_block_Dep"] = None
    right: Option["catchain_block_Dep"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class catchain_block_data_nop(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd0\xb4\x82\x54"
    tl_type: typing.Literal["catchain.block.data.nop"] = pydantic.Field(
        alias="@type", default="catchain.block.data.nop"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_block_inner_Data = typing.Annotated[
    catchain_block_data_badBlock | catchain_block_data_fork | catchain_block_data_nop,
    pydantic.Field(discriminator="tl_type"),
]
catchain_block_inner_Data_Model = pydantic.RootModel[catchain_block_inner_Data]


# ===== catchain.config.Global =====
class catchain_config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x51\xb6\xc7\x68"
    tl_type: typing.Literal["catchain.config.global"] = pydantic.Field(
        alias="@type", default="catchain.config.global"
    )
    tag: Int256 = b"\x00" * 32
    nodes: list["PublicKey"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type catchain_config_Global = catchain_config_global
catchain_config_Global_Model = pydantic.RootModel[catchain_config_Global]


# ===== config.Global =====
class config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb0\xe9\x66\xf0"
    tl_type: typing.Literal["config.global"] = pydantic.Field(
        alias="@type", default="config.global"
    )
    adnl: Option["adnl_config_Global"] = None
    dht: Option["dht_config_Global"] = None
    validator: Option["validator_config_Global"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type config_Global = config_global
config_Global_Model = pydantic.RootModel[config_Global]


# ===== config.Local =====
class config_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5c\x91\x9e\x78"
    tl_type: typing.Literal["config.local"] = pydantic.Field(alias="@type", default="config.local")
    local_ids: list["id_config_Local"] = []
    dht: list["dht_config_Local"] = []
    validators: list["validator_config_Local"] = []
    liteservers: list["liteserver_config_Local"] = []
    control: list["control_config_Local"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type config_Local = config_local
config_Local_Model = pydantic.RootModel[config_Local]


# ===== control.config.Local =====
class control_config_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xed\xec\x1d\x75"
    tl_type: typing.Literal["control.config.local"] = pydantic.Field(
        alias="@type", default="control.config.local"
    )
    priv: Option["PrivateKey"] = None
    pub: Int256 = b"\x00" * 32
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type control_config_Local = control_config_local
control_config_Local_Model = pydantic.RootModel[control_config_Local]


# ===== db.Candidate =====
class db_candidate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xda\x6a\xd9\x65"
    tl_type: typing.Literal["db.candidate"] = pydantic.Field(alias="@type", default="db.candidate")
    source: Option["PublicKey"] = None
    id: Option["tonNode_BlockIdExt"] = None
    data: pydantic.Base64Bytes = b""
    collated_data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_Candidate = db_candidate
db_Candidate_Model = pydantic.RootModel[db_Candidate]


# ===== db.block.Info =====
class db_block_packedInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x92\x91\xbb\x46"
    tl_type: typing.Literal["db.block.packedInfo"] = pydantic.Field(
        alias="@type", default="db.block.packedInfo"
    )
    id: Option["tonNode_BlockIdExt"] = None
    unixtime: int = 0
    offset: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_block_Info = db_block_packedInfo
db_block_Info_Model = pydantic.RootModel[db_block_Info]


# ===== db.blockdb.Key =====
class db_blockdb_key_lru(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3a\x96\xbc\x50"
    tl_type: typing.Literal["db.blockdb.key.lru"] = pydantic.Field(
        alias="@type", default="db.blockdb.key.lru"
    )
    id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_blockdb_key_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x73\xd1\x57\x7f"
    tl_type: typing.Literal["db.blockdb.key.value"] = pydantic.Field(
        alias="@type", default="db.blockdb.key.value"
    )
    id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_blockdb_Key = typing.Annotated[
    db_blockdb_key_lru | db_blockdb_key_value, pydantic.Field(discriminator="tl_type")
]
db_blockdb_Key_Model = pydantic.RootModel[db_blockdb_Key]


# ===== db.blockdb.Lru =====
class db_blockdb_lru(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb3\x55\x16\xc1"
    tl_type: typing.Literal["db.blockdb.lru"] = pydantic.Field(
        alias="@type", default="db.blockdb.lru"
    )
    id: Option["tonNode_BlockIdExt"] = None
    prev: Int256 = b"\x00" * 32
    next: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_blockdb_Lru = db_blockdb_lru
db_blockdb_Lru_Model = pydantic.RootModel[db_blockdb_Lru]


# ===== db.blockdb.Value =====
class db_blockdb_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2d\xc4\x8e\xb2"
    tl_type: typing.Literal["db.blockdb.value"] = pydantic.Field(
        alias="@type", default="db.blockdb.value"
    )
    next: Option["tonNode_BlockIdExt"] = None
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_blockdb_Value = db_blockdb_value
db_blockdb_Value_Model = pydantic.RootModel[db_blockdb_Value]


# ===== db.candidate.Id =====
class db_candidate_id(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x87\xb2\xc0\x37"
    tl_type: typing.Literal["db.candidate.id"] = pydantic.Field(
        alias="@type", default="db.candidate.id"
    )
    source: Option["PublicKey"] = None
    id: Option["tonNode_BlockIdExt"] = None
    collated_data_file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_candidate_Id = db_candidate_id
db_candidate_Id_Model = pydantic.RootModel[db_candidate_Id]


# ===== db.celldb.Value =====
class db_celldb_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x40\x14\x10\xe6"
    tl_type: typing.Literal["db.celldb.value"] = pydantic.Field(
        alias="@type", default="db.celldb.value"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    prev: Int256 = b"\x00" * 32
    next: Int256 = b"\x00" * 32
    root_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_celldb_Value = db_celldb_value
db_celldb_Value_Model = pydantic.RootModel[db_celldb_Value]


# ===== db.celldb.key.Value =====
class db_celldb_key_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x23\x39\xb1\x5b"
    tl_type: typing.Literal["db.celldb.key.value"] = pydantic.Field(
        alias="@type", default="db.celldb.key.value"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_celldb_key_Value = db_celldb_key_value
db_celldb_key_Value_Model = pydantic.RootModel[db_celldb_key_Value]


# ===== db.filedb.Key =====
class db_filedb_key_empty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4b\x27\xff\x7b"
    tl_type: typing.Literal["db.filedb.key.empty"] = pydantic.Field(
        alias="@type", default="db.filedb.key.empty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_blockFile(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x71\xe4\xea\xb0"
    tl_type: typing.Literal["db.filedb.key.blockFile"] = pydantic.Field(
        alias="@type", default="db.filedb.key.blockFile"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_zeroStateFile(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3d\x86\x52\x12"
    tl_type: typing.Literal["db.filedb.key.zeroStateFile"] = pydantic.Field(
        alias="@type", default="db.filedb.key.zeroStateFile"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_persistentStateFile(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4c\x76\xb6\xaf"
    tl_type: typing.Literal["db.filedb.key.persistentStateFile"] = pydantic.Field(
        alias="@type", default="db.filedb.key.persistentStateFile"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    masterchain_block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_splitAccountStateFile(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x66\xa5\x6c\xc6"
    tl_type: typing.Literal["db.filedb.key.splitAccountStateFile"] = pydantic.Field(
        alias="@type", default="db.filedb.key.splitAccountStateFile"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    masterchain_block_id: Option["tonNode_BlockIdExt"] = None
    effective_shard: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_splitPersistentStateFile(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5f\x33\xb0\x9b"
    tl_type: typing.Literal["db.filedb.key.splitPersistentStateFile"] = pydantic.Field(
        alias="@type", default="db.filedb.key.splitPersistentStateFile"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    masterchain_block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_proof(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xec\x4d\x95\xda"
    tl_type: typing.Literal["db.filedb.key.proof"] = pydantic.Field(
        alias="@type", default="db.filedb.key.proof"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_proofLink(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xce\xc5\xfb\x98"
    tl_type: typing.Literal["db.filedb.key.proofLink"] = pydantic.Field(
        alias="@type", default="db.filedb.key.proofLink"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_signatures(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0b\x0d\x29\xd7"
    tl_type: typing.Literal["db.filedb.key.signatures"] = pydantic.Field(
        alias="@type", default="db.filedb.key.signatures"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_candidate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb9\x0a\x8a\xe2"
    tl_type: typing.Literal["db.filedb.key.candidate"] = pydantic.Field(
        alias="@type", default="db.filedb.key.candidate"
    )
    id: Option["db_candidate_Id"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_candidateRef(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbe\x42\x4d\xe6"
    tl_type: typing.Literal["db.filedb.key.candidateRef"] = pydantic.Field(
        alias="@type", default="db.filedb.key.candidateRef"
    )
    id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_filedb_key_blockInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfc\xd4\x99\xc4"
    tl_type: typing.Literal["db.filedb.key.blockInfo"] = pydantic.Field(
        alias="@type", default="db.filedb.key.blockInfo"
    )
    block_id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_filedb_Key = typing.Annotated[
    db_filedb_key_empty
    | db_filedb_key_blockFile
    | db_filedb_key_zeroStateFile
    | db_filedb_key_persistentStateFile
    | db_filedb_key_splitAccountStateFile
    | db_filedb_key_splitPersistentStateFile
    | db_filedb_key_proof
    | db_filedb_key_proofLink
    | db_filedb_key_signatures
    | db_filedb_key_candidate
    | db_filedb_key_candidateRef
    | db_filedb_key_blockInfo,
    pydantic.Field(discriminator="tl_type"),
]
db_filedb_Key_Model = pydantic.RootModel[db_filedb_Key]


# ===== db.filedb.Value =====
class db_filedb_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2d\x1a\xdd\xf2"
    tl_type: typing.Literal["db.filedb.value"] = pydantic.Field(
        alias="@type", default="db.filedb.value"
    )
    key: Option["db_filedb_Key"] = None
    prev: Int256 = b"\x00" * 32
    next: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_filedb_Value = db_filedb_value
db_filedb_Value_Model = pydantic.RootModel[db_filedb_Value]


# ===== db.files.Key =====
class db_files_index_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x02\x05\xc4\x7d"
    tl_type: typing.Literal["db.files.index.key"] = pydantic.Field(
        alias="@type", default="db.files.index.key"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_files_package_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3e\x03\x04\xa5"
    tl_type: typing.Literal["db.files.package.key"] = pydantic.Field(
        alias="@type", default="db.files.package.key"
    )
    package_id: int = 0
    key: bool = False
    temp: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_files_Key = typing.Annotated[
    db_files_index_key | db_files_package_key, pydantic.Field(discriminator="tl_type")
]
db_files_Key_Model = pydantic.RootModel[db_files_Key]


# ===== db.files.index.Value =====
class db_files_index_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfc\xda\xb1\xa2"
    tl_type: typing.Literal["db.files.index.value"] = pydantic.Field(
        alias="@type", default="db.files.index.value"
    )
    packages: list[int] = []
    key_packages: list[int] = []
    temp_packages: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_files_index_Value = db_files_index_value
db_files_index_Value_Model = pydantic.RootModel[db_files_index_Value]


# ===== db.files.package.FirstBlock =====
class db_files_package_firstBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe7\x69\x12\x70"
    tl_type: typing.Literal["db.files.package.firstBlock"] = pydantic.Field(
        alias="@type", default="db.files.package.firstBlock"
    )
    workchain: int = 0
    shard: int = 0
    seqno: int = 0
    unixtime: int = 0
    lt: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_files_package_FirstBlock = db_files_package_firstBlock
db_files_package_FirstBlock_Model = pydantic.RootModel[db_files_package_FirstBlock]


# ===== db.files.package.Value =====
class db_files_package_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2b\xd5\x4c\xe4"
    tl_type: typing.Literal["db.files.package.value"] = pydantic.Field(
        alias="@type", default="db.files.package.value"
    )
    package_id: int = 0
    key: bool = False
    temp: bool = False
    firstblocks: list["db_files_package_FirstBlock"] = []
    deleted: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_files_package_Value = db_files_package_value
db_files_package_Value_Model = pydantic.RootModel[db_files_package_Value]


# ===== db.lt.Key =====
class db_lt_el_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe2\x1a\x32\xa5"
    tl_type: typing.Literal["db.lt.el.key"] = pydantic.Field(alias="@type", default="db.lt.el.key")
    workchain: int = 0
    shard: int = 0
    idx: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_lt_desc_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x91\xe7\xe3\xf1"
    tl_type: typing.Literal["db.lt.desc.key"] = pydantic.Field(
        alias="@type", default="db.lt.desc.key"
    )
    workchain: int = 0
    shard: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_lt_shard_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0f\xf9\xa6\x50"
    tl_type: typing.Literal["db.lt.shard.key"] = pydantic.Field(
        alias="@type", default="db.lt.shard.key"
    )
    idx: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_lt_status_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x57\x60\x6c\x77"
    tl_type: typing.Literal["db.lt.status.key"] = pydantic.Field(
        alias="@type", default="db.lt.status.key"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_lt_Key = typing.Annotated[
    db_lt_el_key | db_lt_desc_key | db_lt_shard_key | db_lt_status_key,
    pydantic.Field(discriminator="tl_type"),
]
db_lt_Key_Model = pydantic.RootModel[db_lt_Key]


# ===== db.lt.desc.Value =====
class db_lt_desc_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb4\x51\xaf\x71"
    tl_type: typing.Literal["db.lt.desc.value"] = pydantic.Field(
        alias="@type", default="db.lt.desc.value"
    )
    first_idx: int = 0
    last_idx: int = 0
    last_seqno: int = 0
    last_lt: int = 0
    last_ts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_lt_desc_Value = db_lt_desc_value
db_lt_desc_Value_Model = pydantic.RootModel[db_lt_desc_Value]


# ===== db.lt.el.Value =====
class db_lt_el_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x64\x5f\xe6\x95"
    tl_type: typing.Literal["db.lt.el.value"] = pydantic.Field(
        alias="@type", default="db.lt.el.value"
    )
    id: Option["tonNode_BlockIdExt"] = None
    lt: int = 0
    ts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_lt_el_Value = db_lt_el_value
db_lt_el_Value_Model = pydantic.RootModel[db_lt_el_Value]


# ===== db.lt.shard.Value =====
class db_lt_shard_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7b\x9a\x73\x3c"
    tl_type: typing.Literal["db.lt.shard.value"] = pydantic.Field(
        alias="@type", default="db.lt.shard.value"
    )
    workchain: int = 0
    shard: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_lt_shard_Value = db_lt_shard_value
db_lt_shard_Value_Model = pydantic.RootModel[db_lt_shard_Value]


# ===== db.lt.status.Value =====
class db_lt_status_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x39\xed\xbe\xfa"
    tl_type: typing.Literal["db.lt.status.value"] = pydantic.Field(
        alias="@type", default="db.lt.status.value"
    )
    total_shards: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_lt_status_Value = db_lt_status_value
db_lt_status_Value_Model = pydantic.RootModel[db_lt_status_Value]


# ===== db.root.Config =====
class db_root_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa1\x82\x11\xd6"
    tl_type: typing.Literal["db.root.config"] = pydantic.Field(
        alias="@type", default="db.root.config"
    )
    celldb_version: int = 0
    blockdb_version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_root_Config = db_root_config
db_root_Config_Model = pydantic.RootModel[db_root_Config]


# ===== db.root.DbDescription =====
class db_root_dbDescription(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf3\x73\x18\xb4"
    tl_type: typing.Literal["db.root.dbDescription"] = pydantic.Field(
        alias="@type", default="db.root.dbDescription"
    )
    version: int = 0
    first_masterchain_block_id: Option["tonNode_BlockIdExt"] = None
    flags: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_root_DbDescription = db_root_dbDescription
db_root_DbDescription_Model = pydantic.RootModel[db_root_DbDescription]


# ===== db.root.Key =====
class db_root_key_cellDb(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3e\xb3\xf9\x72"
    tl_type: typing.Literal["db.root.key.cellDb"] = pydantic.Field(
        alias="@type", default="db.root.key.cellDb"
    )
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_root_key_blockDb(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x40\xbf\x12\x30"
    tl_type: typing.Literal["db.root.key.blockDb"] = pydantic.Field(
        alias="@type", default="db.root.key.blockDb"
    )
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_root_key_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\x32\xc3\x13"
    tl_type: typing.Literal["db.root.key.config"] = pydantic.Field(
        alias="@type", default="db.root.key.config"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_root_Key = typing.Annotated[
    db_root_key_cellDb | db_root_key_blockDb | db_root_key_config,
    pydantic.Field(discriminator="tl_type"),
]
db_root_Key_Model = pydantic.RootModel[db_root_Key]


# ===== db.state.AsyncSerializer =====
class db_state_asyncSerializer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa1\x29\x2f\xd3"
    tl_type: typing.Literal["db.state.asyncSerializer"] = pydantic.Field(
        alias="@type", default="db.state.asyncSerializer"
    )
    block: Option["tonNode_BlockIdExt"] = None
    last: Option["tonNode_BlockIdExt"] = None
    last_ts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_AsyncSerializer = db_state_asyncSerializer
db_state_AsyncSerializer_Model = pydantic.RootModel[db_state_AsyncSerializer]


# ===== db.state.DbVersion =====
class db_state_dbVersion(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf7\x20\x37\xd9"
    tl_type: typing.Literal["db.state.dbVersion"] = pydantic.Field(
        alias="@type", default="db.state.dbVersion"
    )
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_DbVersion = db_state_dbVersion
db_state_DbVersion_Model = pydantic.RootModel[db_state_DbVersion]


# ===== db.state.DestroyedSessions =====
class db_state_destroyedSessions(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\xd9\xa8\xad"
    tl_type: typing.Literal["db.state.destroyedSessions"] = pydantic.Field(
        alias="@type", default="db.state.destroyedSessions"
    )
    sessions: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_DestroyedSessions = db_state_destroyedSessions
db_state_DestroyedSessions_Model = pydantic.RootModel[db_state_DestroyedSessions]


# ===== db.state.GcBlockId =====
class db_state_gcBlockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\xbd\x30\xdf"
    tl_type: typing.Literal["db.state.gcBlockId"] = pydantic.Field(
        alias="@type", default="db.state.gcBlockId"
    )
    block: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_GcBlockId = db_state_gcBlockId
db_state_GcBlockId_Model = pydantic.RootModel[db_state_GcBlockId]


# ===== db.state.Hardforks =====
class db_state_hardforks(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x04\x0d\xf3\x85"
    tl_type: typing.Literal["db.state.hardforks"] = pydantic.Field(
        alias="@type", default="db.state.hardforks"
    )
    blocks: list["tonNode_BlockIdExt"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_Hardforks = db_state_hardforks
db_state_Hardforks_Model = pydantic.RootModel[db_state_Hardforks]


# ===== db.state.InitBlockId =====
class db_state_initBlockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf5\x9c\x2c\x73"
    tl_type: typing.Literal["db.state.initBlockId"] = pydantic.Field(
        alias="@type", default="db.state.initBlockId"
    )
    block: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_InitBlockId = db_state_initBlockId
db_state_InitBlockId_Model = pydantic.RootModel[db_state_InitBlockId]


# ===== db.state.Key =====
class db_state_key_destroyedSessions(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x59\xf1\xf7\xe8"
    tl_type: typing.Literal["db.state.key.destroyedSessions"] = pydantic.Field(
        alias="@type", default="db.state.key.destroyedSessions"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_initBlockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe3\x78\x82\x75"
    tl_type: typing.Literal["db.state.key.initBlockId"] = pydantic.Field(
        alias="@type", default="db.state.key.initBlockId"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_gcBlockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xde\xf3\x79\xc3"
    tl_type: typing.Literal["db.state.key.gcBlockId"] = pydantic.Field(
        alias="@type", default="db.state.key.gcBlockId"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_shardClient(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x87\x31\x9b\xc9"
    tl_type: typing.Literal["db.state.key.shardClient"] = pydantic.Field(
        alias="@type", default="db.state.key.shardClient"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_asyncSerializer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1f\x8a\xae\x29"
    tl_type: typing.Literal["db.state.key.asyncSerializer"] = pydantic.Field(
        alias="@type", default="db.state.key.asyncSerializer"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_hardforks(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xba\x27\xf4\xe6"
    tl_type: typing.Literal["db.state.key.hardforks"] = pydantic.Field(
        alias="@type", default="db.state.key.hardforks"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_dbVersion(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x54\x21\x4f\x72"
    tl_type: typing.Literal["db.state.key.dbVersion"] = pydantic.Field(
        alias="@type", default="db.state.key.dbVersion"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_persistentStateDescriptionShards(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x88\xc7\x9b\x91"
    tl_type: typing.Literal["db.state.key.persistentStateDescriptionShards"] = pydantic.Field(
        alias="@type", default="db.state.key.persistentStateDescriptionShards"
    )
    masterchain_seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_key_persistentStateDescriptionsList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xca\x79\x17\x5a"
    tl_type: typing.Literal["db.state.key.persistentStateDescriptionsList"] = pydantic.Field(
        alias="@type", default="db.state.key.persistentStateDescriptionsList"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_Key = typing.Annotated[
    db_state_key_destroyedSessions
    | db_state_key_initBlockId
    | db_state_key_gcBlockId
    | db_state_key_shardClient
    | db_state_key_asyncSerializer
    | db_state_key_hardforks
    | db_state_key_dbVersion
    | db_state_key_persistentStateDescriptionShards
    | db_state_key_persistentStateDescriptionsList,
    pydantic.Field(discriminator="tl_type"),
]
db_state_Key_Model = pydantic.RootModel[db_state_Key]


# ===== db.state.PersistentStateDescriptionHeader =====
class db_state_persistentStateDescriptionHeader(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x54\x23\x04\xb1"
    tl_type: typing.Literal["db.state.persistentStateDescriptionHeader"] = pydantic.Field(
        alias="@type", default="db.state.persistentStateDescriptionHeader"
    )
    masterchain_id: Option["tonNode_BlockIdExt"] = None
    start_time: int = 0
    end_time: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_PersistentStateDescriptionHeader = db_state_persistentStateDescriptionHeader
db_state_PersistentStateDescriptionHeader_Model = pydantic.RootModel[
    db_state_PersistentStateDescriptionHeader
]


# ===== db.state.PersistentStateDescriptionShard =====
class db_state_persistentStateDescriptionShard(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x49\x56\xfa\x92"
    tl_type: typing.Literal["db.state.persistentStateDescriptionShard"] = pydantic.Field(
        alias="@type", default="db.state.persistentStateDescriptionShard"
    )
    block: Option["tonNode_BlockIdExt"] = None
    split_depth: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_PersistentStateDescriptionShard = db_state_persistentStateDescriptionShard
db_state_PersistentStateDescriptionShard_Model = pydantic.RootModel[
    db_state_PersistentStateDescriptionShard
]


# ===== db.state.PersistentStateDescriptionShards =====
class db_state_persistentStateDescriptionShards(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x50\x94\x30\xe6"
    tl_type: typing.Literal["db.state.persistentStateDescriptionShards"] = pydantic.Field(
        alias="@type", default="db.state.persistentStateDescriptionShards"
    )
    shard_blocks: list["tonNode_BlockIdExt"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class db_state_persistentStateDescriptionShardsV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\x96\x38\xdc"
    tl_type: typing.Literal["db.state.persistentStateDescriptionShardsV2"] = pydantic.Field(
        alias="@type", default="db.state.persistentStateDescriptionShardsV2"
    )
    shard_blocks: list["db_state_PersistentStateDescriptionShard"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_PersistentStateDescriptionShards = typing.Annotated[
    db_state_persistentStateDescriptionShards | db_state_persistentStateDescriptionShardsV2,
    pydantic.Field(discriminator="tl_type"),
]
db_state_PersistentStateDescriptionShards_Model = pydantic.RootModel[
    db_state_PersistentStateDescriptionShards
]


# ===== db.state.PersistentStateDescriptionsList =====
class db_state_persistentStateDescriptionsList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf9\xb6\x77\xa7"
    tl_type: typing.Literal["db.state.persistentStateDescriptionsList"] = pydantic.Field(
        alias="@type", default="db.state.persistentStateDescriptionsList"
    )
    list_: list["db_state_PersistentStateDescriptionHeader"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_PersistentStateDescriptionsList = db_state_persistentStateDescriptionsList
db_state_PersistentStateDescriptionsList_Model = pydantic.RootModel[
    db_state_PersistentStateDescriptionsList
]


# ===== db.state.ShardClient =====
class db_state_shardClient(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9d\xa6\x16\x0b"
    tl_type: typing.Literal["db.state.shardClient"] = pydantic.Field(
        alias="@type", default="db.state.shardClient"
    )
    block: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type db_state_ShardClient = db_state_shardClient
db_state_ShardClient_Model = pydantic.RootModel[db_state_ShardClient]


# ===== dht.Key =====
class dht_key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8f\xde\x67\xf6"
    tl_type: typing.Literal["dht.key"] = pydantic.Field(alias="@type", default="dht.key")
    id: Int256 = b"\x00" * 32
    name: pydantic.Base64Bytes = b""
    idx: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Key = dht_key
dht_Key_Model = pydantic.RootModel[dht_Key]


# ===== dht.KeyDescription =====
class dht_keyDescription(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x05\x4e\x1d\x28"
    tl_type: typing.Literal["dht.keyDescription"] = pydantic.Field(
        alias="@type", default="dht.keyDescription"
    )
    key: Option["dht_Key"] = None
    id: Option["PublicKey"] = None
    update_rule: Option["dht_UpdateRule"] = None
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_KeyDescription = dht_keyDescription
dht_KeyDescription_Model = pydantic.RootModel[dht_KeyDescription]


# ===== dht.Message =====
class dht_message(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8e\xdb\x0c\xbc"
    tl_type: typing.Literal["dht.message"] = pydantic.Field(alias="@type", default="dht.message")
    node: Option["dht_Node"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Message = dht_message
dht_Message_Model = pydantic.RootModel[dht_Message]


# ===== dht.Node =====
class dht_node(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x48\x32\x53\x84"
    tl_type: typing.Literal["dht.node"] = pydantic.Field(alias="@type", default="dht.node")
    id: Option["PublicKey"] = None
    addr_list: Option["adnl_AddressList"] = None
    version: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Node = dht_node
dht_Node_Model = pydantic.RootModel[dht_Node]


# ===== dht.Nodes =====
class dht_nodes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbe\xa0\x74\x79"
    tl_type: typing.Literal["dht.nodes"] = pydantic.Field(alias="@type", default="dht.nodes")
    nodes: list["dht_Node"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Nodes = dht_nodes
dht_Nodes_Model = pydantic.RootModel[dht_Nodes]


# ===== dht.Pong =====
class dht_pong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x81\xef\x8a\x5a"
    tl_type: typing.Literal["dht.pong"] = pydantic.Field(alias="@type", default="dht.pong")
    random_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Pong = dht_pong
dht_Pong_Model = pydantic.RootModel[dht_Pong]


# ===== dht.RequestReversePingCont =====
class dht_requestReversePingCont(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x05\xc1\xad\xdb"
    tl_type: typing.Literal["dht.requestReversePingCont"] = pydantic.Field(
        alias="@type", default="dht.requestReversePingCont"
    )
    target: Option["adnl_Node"] = None
    signature: pydantic.Base64Bytes = b""
    client: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_RequestReversePingCont = dht_requestReversePingCont
dht_RequestReversePingCont_Model = pydantic.RootModel[dht_RequestReversePingCont]


# ===== dht.ReversePingResult =====
class dht_clientNotFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6f\x7e\x1c\x2d"
    tl_type: typing.Literal["dht.clientNotFound"] = pydantic.Field(
        alias="@type", default="dht.clientNotFound"
    )
    nodes: Option["dht_Nodes"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dht_reversePingOk(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa2\x30\x40\x20"
    tl_type: typing.Literal["dht.reversePingOk"] = pydantic.Field(
        alias="@type", default="dht.reversePingOk"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_ReversePingResult = typing.Annotated[
    dht_clientNotFound | dht_reversePingOk, pydantic.Field(discriminator="tl_type")
]
dht_ReversePingResult_Model = pydantic.RootModel[dht_ReversePingResult]


# ===== dht.Stored =====
class dht_stored(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x08\xfb\x26\x70"
    tl_type: typing.Literal["dht.stored"] = pydantic.Field(alias="@type", default="dht.stored")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Stored = dht_stored
dht_Stored_Model = pydantic.RootModel[dht_Stored]


# ===== dht.UpdateRule =====
class dht_updateRule_signature(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf7\x31\x9f\xcc"
    tl_type: typing.Literal["dht.updateRule.signature"] = pydantic.Field(
        alias="@type", default="dht.updateRule.signature"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dht_updateRule_anybody(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x14\x8e\x57\x61"
    tl_type: typing.Literal["dht.updateRule.anybody"] = pydantic.Field(
        alias="@type", default="dht.updateRule.anybody"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dht_updateRule_overlayNodes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x83\x93\x77\x26"
    tl_type: typing.Literal["dht.updateRule.overlayNodes"] = pydantic.Field(
        alias="@type", default="dht.updateRule.overlayNodes"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_UpdateRule = typing.Annotated[
    dht_updateRule_signature | dht_updateRule_anybody | dht_updateRule_overlayNodes,
    pydantic.Field(discriminator="tl_type"),
]
dht_UpdateRule_Model = pydantic.RootModel[dht_UpdateRule]


# ===== dht.Value =====
class dht_value(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcb\x27\xad\x90"
    tl_type: typing.Literal["dht.value"] = pydantic.Field(alias="@type", default="dht.value")
    key: Option["dht_KeyDescription"] = None
    value: pydantic.Base64Bytes = b""
    ttl: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_Value = dht_value
dht_Value_Model = pydantic.RootModel[dht_Value]


# ===== dht.ValueResult =====
class dht_valueNotFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x68\x05\x62\xa2"
    tl_type: typing.Literal["dht.valueNotFound"] = pydantic.Field(
        alias="@type", default="dht.valueNotFound"
    )
    nodes: Option["dht_Nodes"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dht_valueFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x74\xf7\x0c\xe4"
    tl_type: typing.Literal["dht.valueFound"] = pydantic.Field(
        alias="@type", default="dht.valueFound"
    )
    value: Option["dht_Value"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_ValueResult = typing.Annotated[
    dht_valueNotFound | dht_valueFound, pydantic.Field(discriminator="tl_type")
]
dht_ValueResult_Model = pydantic.RootModel[dht_ValueResult]


# ===== dht.config.Global =====
class dht_config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x07\xca\xce\x84"
    tl_type: typing.Literal["dht.config.global"] = pydantic.Field(
        alias="@type", default="dht.config.global"
    )
    static_nodes: Option["dht_Nodes"] = None
    k: int = 0
    a: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dht_config_global_v2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x27\x84\x63\x69"
    tl_type: typing.Literal["dht.config.global_v2"] = pydantic.Field(
        alias="@type", default="dht.config.global_v2"
    )
    static_nodes: Option["dht_Nodes"] = None
    k: int = 0
    a: int = 0
    network_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_config_Global = typing.Annotated[
    dht_config_global | dht_config_global_v2, pydantic.Field(discriminator="tl_type")
]
dht_config_Global_Model = pydantic.RootModel[dht_config_Global]


# ===== dht.config.Local =====
class dht_config_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6f\x4a\x20\x76"
    tl_type: typing.Literal["dht.config.local"] = pydantic.Field(
        alias="@type", default="dht.config.local"
    )
    id: Option["adnl_id_Short"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dht_config_random_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x77\x25\xeb\x9b"
    tl_type: typing.Literal["dht.config.random.local"] = pydantic.Field(
        alias="@type", default="dht.config.random.local"
    )
    cnt: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_config_Local = typing.Annotated[
    dht_config_local | dht_config_random_local, pydantic.Field(discriminator="tl_type")
]
dht_config_Local_Model = pydantic.RootModel[dht_config_Local]


# ===== dht.db.Bucket =====
class dht_db_bucket(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6c\xfa\x9c\xb3"
    tl_type: typing.Literal["dht.db.bucket"] = pydantic.Field(
        alias="@type", default="dht.db.bucket"
    )
    nodes: Option["dht_Nodes"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_db_Bucket = dht_db_bucket
dht_db_Bucket_Model = pydantic.RootModel[dht_db_Bucket]


# ===== dht.db.Key =====
class dht_db_key_bucket(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4c\xae\x68\xa3"
    tl_type: typing.Literal["dht.db.key.bucket"] = pydantic.Field(
        alias="@type", default="dht.db.key.bucket"
    )
    id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dht_db_Key = dht_db_key_bucket
dht_db_Key_Model = pydantic.RootModel[dht_db_Key]


# ===== dummyworkchain0.config.Global =====
class dummyworkchain0_config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd3\x6e\x61\xda"
    tl_type: typing.Literal["dummyworkchain0.config.global"] = pydantic.Field(
        alias="@type", default="dummyworkchain0.config.global"
    )
    zero_state_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dummyworkchain0_config_Global = dummyworkchain0_config_global
dummyworkchain0_config_Global_Model = pydantic.RootModel[dummyworkchain0_config_Global]


# ===== engine.Addr =====
class engine_addr(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xec\x1f\x31\xef"
    tl_type: typing.Literal["engine.addr"] = pydantic.Field(alias="@type", default="engine.addr")
    ip: int = 0
    port: int = 0
    categories: list[int] = []
    priority_categories: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class engine_addrProxy(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x49\x65\xdf\x8a"
    tl_type: typing.Literal["engine.addrProxy"] = pydantic.Field(
        alias="@type", default="engine.addrProxy"
    )
    in_ip: int = 0
    in_port: int = 0
    out_ip: int = 0
    out_port: int = 0
    proxy_type: Option["adnl_Proxy"] = None
    categories: list[int] = []
    priority_categories: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_Addr = typing.Annotated[
    engine_addr | engine_addrProxy, pydantic.Field(discriminator="tl_type")
]
engine_Addr_Model = pydantic.RootModel[engine_Addr]


# ===== engine.Adnl =====
class engine_adnl(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x50\x65\xd7\x62"
    tl_type: typing.Literal["engine.adnl"] = pydantic.Field(alias="@type", default="engine.adnl")
    id: Int256 = b"\x00" * 32
    category: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_Adnl = engine_adnl
engine_Adnl_Model = pydantic.RootModel[engine_Adnl]


# ===== engine.ControlInterface =====
class engine_controlInterface(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xab\x6f\x81\x31"
    tl_type: typing.Literal["engine.controlInterface"] = pydantic.Field(
        alias="@type", default="engine.controlInterface"
    )
    id: Int256 = b"\x00" * 32
    port: int = 0
    allowed: list["engine_ControlProcess"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_ControlInterface = engine_controlInterface
engine_ControlInterface_Model = pydantic.RootModel[engine_ControlInterface]


# ===== engine.ControlProcess =====
class engine_controlProcess(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x17\x48\xc0\x6a"
    tl_type: typing.Literal["engine.controlProcess"] = pydantic.Field(
        alias="@type", default="engine.controlProcess"
    )
    id: Int256 = b"\x00" * 32
    permissions: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_ControlProcess = engine_controlProcess
engine_ControlProcess_Model = pydantic.RootModel[engine_ControlProcess]


# ===== engine.Dht =====
class engine_dht(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfa\xf2\xe9\x5d"
    tl_type: typing.Literal["engine.dht"] = pydantic.Field(alias="@type", default="engine.dht")
    id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_Dht = engine_dht
engine_Dht_Model = pydantic.RootModel[engine_Dht]


# ===== engine.Gc =====
class engine_gc(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7b\x98\xbd\xbf"
    tl_type: typing.Literal["engine.gc"] = pydantic.Field(alias="@type", default="engine.gc")
    ids: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_Gc = engine_gc
engine_Gc_Model = pydantic.RootModel[engine_Gc]


# ===== engine.LiteServer =====
class engine_liteServer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfe\x8e\x70\xbb"
    tl_type: typing.Literal["engine.liteServer"] = pydantic.Field(
        alias="@type", default="engine.liteServer"
    )
    id: Int256 = b"\x00" * 32
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_LiteServer = engine_liteServer
engine_LiteServer_Model = pydantic.RootModel[engine_LiteServer]


# ===== engine.Validator =====
class engine_validator(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x29\xea\x5f\x88"
    tl_type: typing.Literal["engine.validator"] = pydantic.Field(
        alias="@type", default="engine.validator"
    )
    id: Int256 = b"\x00" * 32
    temp_keys: list["engine_ValidatorTempKey"] = []
    adnl_addrs: list["engine_ValidatorAdnlAddress"] = []
    election_date: int = 0
    expire_at: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_Validator = engine_validator
engine_Validator_Model = pydantic.RootModel[engine_Validator]


# ===== engine.ValidatorAdnlAddress =====
class engine_validatorAdnlAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbe\x45\x45\xd3"
    tl_type: typing.Literal["engine.validatorAdnlAddress"] = pydantic.Field(
        alias="@type", default="engine.validatorAdnlAddress"
    )
    id: Int256 = b"\x00" * 32
    expire_at: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_ValidatorAdnlAddress = engine_validatorAdnlAddress
engine_ValidatorAdnlAddress_Model = pydantic.RootModel[engine_ValidatorAdnlAddress]


# ===== engine.ValidatorTempKey =====
class engine_validatorTempKey(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xde\xd6\x4a\x5e"
    tl_type: typing.Literal["engine.validatorTempKey"] = pydantic.Field(
        alias="@type", default="engine.validatorTempKey"
    )
    key: Int256 = b"\x00" * 32
    expire_at: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_ValidatorTempKey = engine_validatorTempKey
engine_ValidatorTempKey_Model = pydantic.RootModel[engine_ValidatorTempKey]


# ===== engine.adnlProxy.Config =====
class engine_adnlProxy_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x01\x41\x26\x6e"
    tl_type: typing.Literal["engine.adnlProxy.config"] = pydantic.Field(
        alias="@type", default="engine.adnlProxy.config"
    )
    ports: list["engine_adnlProxy_Port"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_adnlProxy_Config = engine_adnlProxy_config
engine_adnlProxy_Config_Model = pydantic.RootModel[engine_adnlProxy_Config]


# ===== engine.adnlProxy.Port =====
class engine_adnlProxy_port(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4a\x75\x01\xf9"
    tl_type: typing.Literal["engine.adnlProxy.port"] = pydantic.Field(
        alias="@type", default="engine.adnlProxy.port"
    )
    in_port: int = 0
    out_port: int = 0
    dst_ip: int = 0
    dst_port: int = 0
    proxy_type: Option["adnl_Proxy"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_adnlProxy_Port = engine_adnlProxy_port
engine_adnlProxy_Port_Model = pydantic.RootModel[engine_adnlProxy_Port]


# ===== engine.dht.Config =====
class engine_dht_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc6\x80\x3d\xf4"
    tl_type: typing.Literal["engine.dht.config"] = pydantic.Field(
        alias="@type", default="engine.dht.config"
    )
    dht: list["engine_Dht"] = []
    gc: Option["engine_Gc"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_dht_Config = engine_dht_config
engine_dht_Config_Model = pydantic.RootModel[engine_dht_Config]


# ===== engine.validator.CollatorOptions =====
class engine_validator_collatorOptions(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe8\x94\xeb\x90"
    tl_type: typing.Literal["engine.validator.collatorOptions"] = pydantic.Field(
        alias="@type", default="engine.validator.collatorOptions"
    )
    deferring_enabled: bool = False
    defer_messages_after: int = 0
    defer_out_queue_size_limit: int = 0
    dispatch_phase_2_max_total: int = 0
    dispatch_phase_3_max_total: int = 0
    dispatch_phase_2_max_per_initiator: int = 0
    dispatch_phase_3_max_per_initiator: int = 0
    whitelist: list[str] = []
    prioritylist: list[str] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_CollatorOptions = engine_validator_collatorOptions
engine_validator_CollatorOptions_Model = pydantic.RootModel[engine_validator_CollatorOptions]


# ===== engine.validator.Config =====
class engine_validator_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1a\x97\x4f\xc5"
    tl_type: typing.Literal["engine.validator.config"] = pydantic.Field(
        alias="@type", default="engine.validator.config"
    )
    out_port: int = 0
    addrs: list["engine_Addr"] = []
    adnl: list["engine_Adnl"] = []
    dht: list["engine_Dht"] = []
    validators: list["engine_Validator"] = []
    fullnode: Int256 = b"\x00" * 32
    fullnodeslaves: list["engine_validator_FullNodeSlave"] = []
    fullnodemasters: list["engine_validator_FullNodeMaster"] = []
    fullnodeconfig: Option["engine_validator_FullNodeConfig"] = None
    extraconfig: Option["engine_validator_ExtraConfig"] = None
    liteservers: list["engine_LiteServer"] = []
    control: list["engine_ControlInterface"] = []
    shards_to_monitor: list["tonNode_ShardId"] = []
    gc: Option["engine_Gc"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_Config = engine_validator_config
engine_validator_Config_Model = pydantic.RootModel[engine_validator_Config]


# ===== engine.validator.ControlQueryError =====
class engine_validator_controlQueryError(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1f\x9a\x26\x77"
    tl_type: typing.Literal["engine.validator.controlQueryError"] = pydantic.Field(
        alias="@type", default="engine.validator.controlQueryError"
    )
    code: int = 0
    message: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ControlQueryError = engine_validator_controlQueryError
engine_validator_ControlQueryError_Model = pydantic.RootModel[engine_validator_ControlQueryError]


# ===== engine.validator.CustomOverlay =====
class engine_validator_customOverlay(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcd\x7c\x64\xa4"
    tl_type: typing.Literal["engine.validator.customOverlay"] = pydantic.Field(
        alias="@type", default="engine.validator.customOverlay"
    )
    name: str = ""
    nodes: list["engine_validator_CustomOverlayNode"] = []
    sender_shards: list["tonNode_ShardId"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_CustomOverlay = engine_validator_customOverlay
engine_validator_CustomOverlay_Model = pydantic.RootModel[engine_validator_CustomOverlay]


# ===== engine.validator.CustomOverlayNode =====
class engine_validator_customOverlayNode(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xff\xde\x47\x29"
    tl_type: typing.Literal["engine.validator.customOverlayNode"] = pydantic.Field(
        alias="@type", default="engine.validator.customOverlayNode"
    )
    adnl_id: Int256 = b"\x00" * 32
    msg_sender: bool = False
    msg_sender_priority: int = 0
    block_sender: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_CustomOverlayNode = engine_validator_customOverlayNode
engine_validator_CustomOverlayNode_Model = pydantic.RootModel[engine_validator_CustomOverlayNode]


# ===== engine.validator.CustomOverlaysConfig =====
class engine_validator_customOverlaysConfig(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x19\x3b\x7a\x40"
    tl_type: typing.Literal["engine.validator.customOverlaysConfig"] = pydantic.Field(
        alias="@type", default="engine.validator.customOverlaysConfig"
    )
    overlays: list["engine_validator_CustomOverlay"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_CustomOverlaysConfig = engine_validator_customOverlaysConfig
engine_validator_CustomOverlaysConfig_Model = pydantic.RootModel[
    engine_validator_CustomOverlaysConfig
]


# ===== engine.validator.DhtServerStatus =====
class engine_validator_dhtServerStatus(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5e\xe7\x1d\xb1"
    tl_type: typing.Literal["engine.validator.dhtServerStatus"] = pydantic.Field(
        alias="@type", default="engine.validator.dhtServerStatus"
    )
    id: Int256 = b"\x00" * 32
    status: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_DhtServerStatus = engine_validator_dhtServerStatus
engine_validator_DhtServerStatus_Model = pydantic.RootModel[engine_validator_DhtServerStatus]


# ===== engine.validator.DhtServersStatus =====
class engine_validator_dhtServersStatus(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x28\xfd\x38\x2b"
    tl_type: typing.Literal["engine.validator.dhtServersStatus"] = pydantic.Field(
        alias="@type", default="engine.validator.dhtServersStatus"
    )
    servers: list["engine_validator_DhtServerStatus"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_DhtServersStatus = engine_validator_dhtServersStatus
engine_validator_DhtServersStatus_Model = pydantic.RootModel[engine_validator_DhtServersStatus]


# ===== engine.validator.ElectionBid =====
class engine_validator_electionBid(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3d\x7a\xb2\x23"
    tl_type: typing.Literal["engine.validator.electionBid"] = pydantic.Field(
        alias="@type", default="engine.validator.electionBid"
    )
    election_date: int = 0
    perm_key: Int256 = b"\x00" * 32
    adnl_addr: Int256 = b"\x00" * 32
    to_send_payload: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ElectionBid = engine_validator_electionBid
engine_validator_ElectionBid_Model = pydantic.RootModel[engine_validator_ElectionBid]


# ===== engine.validator.ExportedPrivateKeys =====
class engine_validator_exportedPrivateKeys(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x91\x79\x20\xb9"
    tl_type: typing.Literal["engine.validator.exportedPrivateKeys"] = pydantic.Field(
        alias="@type", default="engine.validator.exportedPrivateKeys"
    )
    encrypted_data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ExportedPrivateKeys = engine_validator_exportedPrivateKeys
engine_validator_ExportedPrivateKeys_Model = pydantic.RootModel[
    engine_validator_ExportedPrivateKeys
]


# ===== engine.validator.ExtraConfig =====
class engine_validator_extraConfig(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x46\x16\x30\x51"
    tl_type: typing.Literal["engine.validator.extraConfig"] = pydantic.Field(
        alias="@type", default="engine.validator.extraConfig"
    )
    state_serializer_enabled: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ExtraConfig = engine_validator_extraConfig
engine_validator_ExtraConfig_Model = pydantic.RootModel[engine_validator_ExtraConfig]


# ===== engine.validator.FullNodeConfig =====
class engine_validator_fullNodeConfig(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x14\xb1\xfe\x29"
    tl_type: typing.Literal["engine.validator.fullNodeConfig"] = pydantic.Field(
        alias="@type", default="engine.validator.fullNodeConfig"
    )
    ext_messages_broadcast_disabled: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_FullNodeConfig = engine_validator_fullNodeConfig
engine_validator_FullNodeConfig_Model = pydantic.RootModel[engine_validator_FullNodeConfig]


# ===== engine.validator.FullNodeMaster =====
class engine_validator_fullNodeMaster(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x68\xf6\x85\x84"
    tl_type: typing.Literal["engine.validator.fullNodeMaster"] = pydantic.Field(
        alias="@type", default="engine.validator.fullNodeMaster"
    )
    port: int = 0
    adnl: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_FullNodeMaster = engine_validator_fullNodeMaster
engine_validator_FullNodeMaster_Model = pydantic.RootModel[engine_validator_FullNodeMaster]


# ===== engine.validator.FullNodeSlave =====
class engine_validator_fullNodeSlave(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x79\x6b\x25\x88"
    tl_type: typing.Literal["engine.validator.fullNodeSlave"] = pydantic.Field(
        alias="@type", default="engine.validator.fullNodeSlave"
    )
    ip: int = 0
    port: int = 0
    adnl: Option["PublicKey"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_FullNodeSlave = engine_validator_fullNodeSlave
engine_validator_FullNodeSlave_Model = pydantic.RootModel[engine_validator_FullNodeSlave]


# ===== engine.validator.GroupMember =====
class validator_groupMember(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe4\x65\x94\x8b"
    tl_type: typing.Literal["validator.groupMember"] = pydantic.Field(
        alias="@type", default="validator.groupMember"
    )
    public_key_hash: Int256 = b"\x00" * 32
    adnl: Int256 = b"\x00" * 32
    weight: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_GroupMember = validator_groupMember
engine_validator_GroupMember_Model = pydantic.RootModel[engine_validator_GroupMember]


# ===== engine.validator.JsonConfig =====
class engine_validator_jsonConfig(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0b\x92\x2d\x13"
    tl_type: typing.Literal["engine.validator.jsonConfig"] = pydantic.Field(
        alias="@type", default="engine.validator.jsonConfig"
    )
    data: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_JsonConfig = engine_validator_jsonConfig
engine_validator_JsonConfig_Model = pydantic.RootModel[engine_validator_JsonConfig]


# ===== engine.validator.KeyHash =====
class engine_validator_keyHash(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4e\xa5\xc6\xc2"
    tl_type: typing.Literal["engine.validator.keyHash"] = pydantic.Field(
        alias="@type", default="engine.validator.keyHash"
    )
    key_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_KeyHash = engine_validator_keyHash
engine_validator_KeyHash_Model = pydantic.RootModel[engine_validator_KeyHash]


# ===== engine.validator.OnePerfTimerStat =====
class engine_validator_onePerfTimerStat(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x68\xa3\x23\x91"
    tl_type: typing.Literal["engine.validator.onePerfTimerStat"] = pydantic.Field(
        alias="@type", default="engine.validator.onePerfTimerStat"
    )
    time: int = 0
    min: float = 0
    avg: float = 0
    max: float = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_OnePerfTimerStat = engine_validator_onePerfTimerStat
engine_validator_OnePerfTimerStat_Model = pydantic.RootModel[engine_validator_OnePerfTimerStat]


# ===== engine.validator.OneStat =====
class engine_validator_oneStat(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xed\x3a\x98\xa4"
    tl_type: typing.Literal["engine.validator.oneStat"] = pydantic.Field(
        alias="@type", default="engine.validator.oneStat"
    )
    key: str = ""
    value: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_OneStat = engine_validator_oneStat
engine_validator_OneStat_Model = pydantic.RootModel[engine_validator_OneStat]


# ===== engine.validator.OverlayStats =====
class engine_validator_overlayStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4b\xf5\xb4\x6f"
    tl_type: typing.Literal["engine.validator.overlayStats"] = pydantic.Field(
        alias="@type", default="engine.validator.overlayStats"
    )
    overlay_id: Int256 = b"\x00" * 32
    overlay_id_full: Option["PublicKey"] = None
    adnl_id: Int256 = b"\x00" * 32
    scope: str = ""
    nodes: list["engine_validator_OverlayStatsNode"] = []
    stats: list["engine_validator_OneStat"] = []
    total_traffic: Option["engine_validator_OverlayStatsTraffic"] = None
    total_traffic_responses: Option["engine_validator_OverlayStatsTraffic"] = None
    extra: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_OverlayStats = engine_validator_overlayStats
engine_validator_OverlayStats_Model = pydantic.RootModel[engine_validator_OverlayStats]


# ===== engine.validator.OverlayStatsNode =====
class engine_validator_overlayStatsNode(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa3\xaa\xa5\xd9"
    tl_type: typing.Literal["engine.validator.overlayStatsNode"] = pydantic.Field(
        alias="@type", default="engine.validator.overlayStatsNode"
    )
    adnl_id: Int256 = b"\x00" * 32
    ip_addr: str = ""
    is_neighbour: bool = False
    is_alive: bool = False
    node_flags: int = 0
    bdcst_errors: int = 0
    fec_bdcst_errors: int = 0
    last_in_query: int = 0
    last_out_query: int = 0
    traffic: Option["engine_validator_OverlayStatsTraffic"] = None
    traffic_responses: Option["engine_validator_OverlayStatsTraffic"] = None
    last_ping_at: float = 0
    last_ping_time: float = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_OverlayStatsNode = engine_validator_overlayStatsNode
engine_validator_OverlayStatsNode_Model = pydantic.RootModel[engine_validator_OverlayStatsNode]


# ===== engine.validator.OverlayStatsTraffic =====
class engine_validator_overlayStatsTraffic(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe6\x06\x18\x59"
    tl_type: typing.Literal["engine.validator.overlayStatsTraffic"] = pydantic.Field(
        alias="@type", default="engine.validator.overlayStatsTraffic"
    )
    t_out_bytes: int = 0
    t_in_bytes: int = 0
    t_out_pckts: int = 0
    t_in_pckts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_OverlayStatsTraffic = engine_validator_overlayStatsTraffic
engine_validator_OverlayStatsTraffic_Model = pydantic.RootModel[
    engine_validator_OverlayStatsTraffic
]


# ===== engine.validator.OverlaysStats =====
class engine_validator_overlaysStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7f\x26\x09\x9c"
    tl_type: typing.Literal["engine.validator.overlaysStats"] = pydantic.Field(
        alias="@type", default="engine.validator.overlaysStats"
    )
    overlays: list["engine_validator_OverlayStats"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_OverlaysStats = engine_validator_overlaysStats
engine_validator_OverlaysStats_Model = pydantic.RootModel[engine_validator_OverlaysStats]


# ===== engine.validator.PerfTimerStats =====
class engine_validator_perfTimerStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1b\x55\xd0\x5f"
    tl_type: typing.Literal["engine.validator.perfTimerStats"] = pydantic.Field(
        alias="@type", default="engine.validator.perfTimerStats"
    )
    stats: list["engine_validator_PerfTimerStatsByName"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_PerfTimerStats = engine_validator_perfTimerStats
engine_validator_PerfTimerStats_Model = pydantic.RootModel[engine_validator_PerfTimerStats]


# ===== engine.validator.PerfTimerStatsByName =====
class engine_validator_perfTimerStatsByName(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe4\xcd\xba\x82"
    tl_type: typing.Literal["engine.validator.perfTimerStatsByName"] = pydantic.Field(
        alias="@type", default="engine.validator.perfTimerStatsByName"
    )
    name: str = ""
    stats: list["engine_validator_OnePerfTimerStat"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_PerfTimerStatsByName = engine_validator_perfTimerStatsByName
engine_validator_PerfTimerStatsByName_Model = pydantic.RootModel[
    engine_validator_PerfTimerStatsByName
]


# ===== engine.validator.ProposalVote =====
class engine_validator_proposalVote(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xed\x26\x66\x7f"
    tl_type: typing.Literal["engine.validator.proposalVote"] = pydantic.Field(
        alias="@type", default="engine.validator.proposalVote"
    )
    perm_key: Int256 = b"\x00" * 32
    to_send: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ProposalVote = engine_validator_proposalVote
engine_validator_ProposalVote_Model = pydantic.RootModel[engine_validator_ProposalVote]


# ===== engine.validator.ShardOutQueueSize =====
class engine_validator_shardOutQueueSize(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfb\x2f\x13\x7d"
    tl_type: typing.Literal["engine.validator.shardOutQueueSize"] = pydantic.Field(
        alias="@type", default="engine.validator.shardOutQueueSize"
    )
    size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ShardOutQueueSize = engine_validator_shardOutQueueSize
engine_validator_ShardOutQueueSize_Model = pydantic.RootModel[engine_validator_ShardOutQueueSize]


# ===== engine.validator.ShardOverlayStats =====
class engine_validator_shardOverlayStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf0\x1a\x75\x43"
    tl_type: typing.Literal["engine.validator.shardOverlayStats"] = pydantic.Field(
        alias="@type", default="engine.validator.shardOverlayStats"
    )
    shard: str = ""
    active: bool = False
    neighbours: list["engine_validator_shardOverlayStats_Neighbour"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_ShardOverlayStats = engine_validator_shardOverlayStats
engine_validator_ShardOverlayStats_Model = pydantic.RootModel[engine_validator_ShardOverlayStats]


# ===== engine.validator.Signature =====
class engine_validator_signature(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x28\x43\x6c\xfb"
    tl_type: typing.Literal["engine.validator.signature"] = pydantic.Field(
        alias="@type", default="engine.validator.signature"
    )
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_Signature = engine_validator_signature
engine_validator_Signature_Model = pydantic.RootModel[engine_validator_Signature]


# ===== engine.validator.Stats =====
class engine_validator_stats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6f\xd3\x49\x5d"
    tl_type: typing.Literal["engine.validator.stats"] = pydantic.Field(
        alias="@type", default="engine.validator.stats"
    )
    stats: list["engine_validator_OneStat"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_Stats = engine_validator_stats
engine_validator_Stats_Model = pydantic.RootModel[engine_validator_Stats]


# ===== engine.validator.Success =====
class engine_validator_success(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8b\xa6\xe4\xb3"
    tl_type: typing.Literal["engine.validator.success"] = pydantic.Field(
        alias="@type", default="engine.validator.success"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_Success = engine_validator_success
engine_validator_Success_Model = pydantic.RootModel[engine_validator_Success]


# ===== engine.validator.TextStats =====
class engine_validator_textStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x39\x68\xdb\xb9"
    tl_type: typing.Literal["engine.validator.textStats"] = pydantic.Field(
        alias="@type", default="engine.validator.textStats"
    )
    data: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_TextStats = engine_validator_textStats
engine_validator_TextStats_Model = pydantic.RootModel[engine_validator_TextStats]


# ===== engine.validator.Time =====
class engine_validator_time(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfe\xa1\x5f\xdf"
    tl_type: typing.Literal["engine.validator.time"] = pydantic.Field(
        alias="@type", default="engine.validator.time"
    )
    time: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type engine_validator_Time = engine_validator_time
engine_validator_Time_Model = pydantic.RootModel[engine_validator_Time]
type engine_validator_shardOverlayStats_Neighbour = None  # unsupported


# ===== fec.Type =====
class fec_raptorQ(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe0\xa7\x93\x8b"
    tl_type: typing.Literal["fec.raptorQ"] = pydantic.Field(alias="@type", default="fec.raptorQ")
    data_size: int = 0
    symbol_size: int = 0
    symbols_count: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class fec_roundRobin(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe4\x28\xf5\x32"
    tl_type: typing.Literal["fec.roundRobin"] = pydantic.Field(
        alias="@type", default="fec.roundRobin"
    )
    data_size: int = 0
    symbol_size: int = 0
    symbols_count: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class fec_online(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0c\x66\x27\x01"
    tl_type: typing.Literal["fec.online"] = pydantic.Field(alias="@type", default="fec.online")
    data_size: int = 0
    symbol_size: int = 0
    symbols_count: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type fec_Type = typing.Annotated[
    fec_raptorQ | fec_roundRobin | fec_online, pydantic.Field(discriminator="tl_type")
]
fec_Type_Model = pydantic.RootModel[fec_Type]


# ===== http.Header =====
class http_header(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x11\xe5\x9b\x8e"
    tl_type: typing.Literal["http.header"] = pydantic.Field(alias="@type", default="http.header")
    name: str = ""
    value: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_Header = http_header
http_Header_Model = pydantic.RootModel[http_Header]


# ===== http.PayloadPart =====
class http_payloadPart(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x64\xd7\x5a\x29"
    tl_type: typing.Literal["http.payloadPart"] = pydantic.Field(
        alias="@type", default="http.payloadPart"
    )
    data: pydantic.Base64Bytes = b""
    trailer: list["http_Header"] = []
    last: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_PayloadPart = http_payloadPart
http_PayloadPart_Model = pydantic.RootModel[http_PayloadPart]


# ===== http.Response =====
class http_response(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4a\xa7\x48\xca"
    tl_type: typing.Literal["http.response"] = pydantic.Field(
        alias="@type", default="http.response"
    )
    http_version: str = ""
    status_code: int = 0
    reason: str = ""
    headers: list["http_Header"] = []
    no_payload: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_Response = http_response
http_Response_Model = pydantic.RootModel[http_Response]


# ===== http.proxy.Capabilities =====
class http_proxy_capabilities(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x11\x6c\x92\x31"
    tl_type: typing.Literal["http.proxy.capabilities"] = pydantic.Field(
        alias="@type", default="http.proxy.capabilities"
    )
    capabilities: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_proxy_Capabilities = http_proxy_capabilities
http_proxy_Capabilities_Model = pydantic.RootModel[http_proxy_Capabilities]


# ===== http.server.Config =====
class http_server_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfc\x77\x14\x3a"
    tl_type: typing.Literal["http.server.config"] = pydantic.Field(
        alias="@type", default="http.server.config"
    )
    dhs: list["http_server_DnsEntry"] = []
    local_hosts: list["http_server_Host"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_server_Config = http_server_config
http_server_Config_Model = pydantic.RootModel[http_server_Config]


# ===== http.server.DnsEntry =====
class http_server_dnsEntry(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x96\x60\x72\xd8"
    tl_type: typing.Literal["http.server.dnsEntry"] = pydantic.Field(
        alias="@type", default="http.server.dnsEntry"
    )
    domain: str = ""
    addr: Option["adnl_id_Short"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_server_DnsEntry = http_server_dnsEntry
http_server_DnsEntry_Model = pydantic.RootModel[http_server_DnsEntry]


# ===== http.server.Host =====
class http_server_host(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa7\xe2\x7d\xc5"
    tl_type: typing.Literal["http.server.host"] = pydantic.Field(
        alias="@type", default="http.server.host"
    )
    domains: list[str] = []
    ip: int = 0
    port: int = 0
    adnl_id: Option["adnl_id_Short"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type http_server_Host = http_server_host
http_server_Host_Model = pydantic.RootModel[http_server_Host]


# ===== id.config.Local =====
class id_config_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8e\xc7\xa9\x92"
    tl_type: typing.Literal["id.config.local"] = pydantic.Field(
        alias="@type", default="id.config.local"
    )
    id: Option["PrivateKey"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type id_config_Local = id_config_local
id_config_Local_Model = pydantic.RootModel[id_config_Local]


# ===== liteclient.config.Global =====
class liteclient_config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xad\x7f\x89\xe8"
    tl_type: typing.Literal["liteclient.config.global"] = pydantic.Field(
        alias="@type", default="liteclient.config.global"
    )
    liteservers: list["liteserver_Desc"] = []
    liteservers_v2: list["liteserver_DescV2"] = []
    validator: Option["validator_config_Global"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteclient_config_Global = liteclient_config_global
liteclient_config_Global_Model = pydantic.RootModel[liteclient_config_Global]


# ===== liteserver.Desc =====
class liteserver_desc(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x74\xa4\x49\xc4"
    tl_type: typing.Literal["liteserver.desc"] = pydantic.Field(
        alias="@type", default="liteserver.desc"
    )
    id: Option["PublicKey"] = None
    ip: int = 0
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteserver_Desc = liteserver_desc
liteserver_Desc_Model = pydantic.RootModel[liteserver_Desc]


# ===== liteserver.DescV2 =====
class liteserver_descV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8b\x1c\x0f\x4b"
    tl_type: typing.Literal["liteserver.descV2"] = pydantic.Field(
        alias="@type", default="liteserver.descV2"
    )
    id: Option["PublicKey"] = None
    ip: int = 0
    port: int = 0
    slices: list["liteserver_descV2_Slice"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteserver_DescV2 = liteserver_descV2
liteserver_DescV2_Model = pydantic.RootModel[liteserver_DescV2]


# ===== liteserver.config.Local =====
class liteserver_config_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8f\xeb\x73\x46"
    tl_type: typing.Literal["liteserver.config.local"] = pydantic.Field(
        alias="@type", default="liteserver.config.local"
    )
    id: Option["PrivateKey"] = None
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class liteserver_config_random_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3b\x45\xc9\x7c"
    tl_type: typing.Literal["liteserver.config.random.local"] = pydantic.Field(
        alias="@type", default="liteserver.config.random.local"
    )
    port: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteserver_config_Local = typing.Annotated[
    liteserver_config_local | liteserver_config_random_local,
    pydantic.Field(discriminator="tl_type"),
]
liteserver_config_Local_Model = pydantic.RootModel[liteserver_config_Local]


# ===== liteserver.descV2.ShardInfo =====
class liteserver_descV2_shardInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa4\xb7\x29\x02"
    tl_type: typing.Literal["liteserver.descV2.shardInfo"] = pydantic.Field(
        alias="@type", default="liteserver.descV2.shardInfo"
    )
    shard_id: Option["tonNode_ShardId"] = None
    seqno: int = 0
    utime: int = 0
    lt: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteserver_descV2_ShardInfo = liteserver_descV2_shardInfo
liteserver_descV2_ShardInfo_Model = pydantic.RootModel[liteserver_descV2_ShardInfo]


# ===== liteserver.descV2.Slice =====
class liteserver_descV2_sliceSimple(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0b\x55\xc9\x88"
    tl_type: typing.Literal["liteserver.descV2.sliceSimple"] = pydantic.Field(
        alias="@type", default="liteserver.descV2.sliceSimple"
    )
    shards: list["tonNode_ShardId"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class liteserver_descV2_sliceTimed(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf2\x2d\x4f\xc3"
    tl_type: typing.Literal["liteserver.descV2.sliceTimed"] = pydantic.Field(
        alias="@type", default="liteserver.descV2.sliceTimed"
    )
    shards_from: list["liteserver_descV2_ShardInfo"] = []
    shards_to: list["liteserver_descV2_ShardInfo"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteserver_descV2_Slice = typing.Annotated[
    liteserver_descV2_sliceSimple | liteserver_descV2_sliceTimed,
    pydantic.Field(discriminator="tl_type"),
]
liteserver_descV2_Slice_Model = pydantic.RootModel[liteserver_descV2_Slice]


# ===== overlay.Broadcast =====
class overlay_fec_received(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xec\x14\x5c\xd5"
    tl_type: typing.Literal["overlay.fec.received"] = pydantic.Field(
        alias="@type", default="overlay.fec.received"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_fec_completed(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x14\x69\xd7\x09"
    tl_type: typing.Literal["overlay.fec.completed"] = pydantic.Field(
        alias="@type", default="overlay.fec.completed"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_unicast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x24\x4e\x53\x33"
    tl_type: typing.Literal["overlay.unicast"] = pydantic.Field(
        alias="@type", default="overlay.unicast"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_broadcast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6b\x2b\x5a\xb1"
    tl_type: typing.Literal["overlay.broadcast"] = pydantic.Field(
        alias="@type", default="overlay.broadcast"
    )
    src: Option["PublicKey"] = None
    certificate: Option["overlay_Certificate"] = None
    flags: int = 0
    data: pydantic.Base64Bytes = b""
    date: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_broadcastFec(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6a\xc3\xd7\xba"
    tl_type: typing.Literal["overlay.broadcastFec"] = pydantic.Field(
        alias="@type", default="overlay.broadcastFec"
    )
    src: Option["PublicKey"] = None
    certificate: Option["overlay_Certificate"] = None
    data_hash: Int256 = b"\x00" * 32
    data_size: int = 0
    flags: int = 0
    data: pydantic.Base64Bytes = b""
    seqno: int = 0
    fec: Option["fec_Type"] = None
    date: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_broadcastFecShort(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x42\x13\x88\xf1"
    tl_type: typing.Literal["overlay.broadcastFecShort"] = pydantic.Field(
        alias="@type", default="overlay.broadcastFecShort"
    )
    src: Option["PublicKey"] = None
    certificate: Option["overlay_Certificate"] = None
    broadcast_hash: Int256 = b"\x00" * 32
    part_data_hash: Int256 = b"\x00" * 32
    seqno: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_broadcastNotFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x24\x36\x86\x95"
    tl_type: typing.Literal["overlay.broadcastNotFound"] = pydantic.Field(
        alias="@type", default="overlay.broadcastNotFound"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_Broadcast = typing.Annotated[
    overlay_fec_received
    | overlay_fec_completed
    | overlay_unicast
    | overlay_broadcast
    | overlay_broadcastFec
    | overlay_broadcastFecShort
    | overlay_broadcastNotFound,
    pydantic.Field(discriminator="tl_type"),
]
overlay_Broadcast_Model = pydantic.RootModel[overlay_Broadcast]


# ===== overlay.BroadcastList =====
class overlay_broadcastList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xdf\xde\xd1\x18"
    tl_type: typing.Literal["overlay.broadcastList"] = pydantic.Field(
        alias="@type", default="overlay.broadcastList"
    )
    hashes: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_BroadcastList = overlay_broadcastList
overlay_BroadcastList_Model = pydantic.RootModel[overlay_BroadcastList]


# ===== overlay.Certificate =====
class overlay_certificate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x31\xd7\x9e\xe0"
    tl_type: typing.Literal["overlay.certificate"] = pydantic.Field(
        alias="@type", default="overlay.certificate"
    )
    issued_by: Option["PublicKey"] = None
    expire_at: int = 0
    max_size: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_certificateV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x83\x9c\x3f\xb4"
    tl_type: typing.Literal["overlay.certificateV2"] = pydantic.Field(
        alias="@type", default="overlay.certificateV2"
    )
    issued_by: Option["PublicKey"] = None
    expire_at: int = 0
    max_size: int = 0
    flags: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_emptyCertificate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcf\xbc\xda\x32"
    tl_type: typing.Literal["overlay.emptyCertificate"] = pydantic.Field(
        alias="@type", default="overlay.emptyCertificate"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_Certificate = typing.Annotated[
    overlay_certificate | overlay_certificateV2 | overlay_emptyCertificate,
    pydantic.Field(discriminator="tl_type"),
]
overlay_Certificate_Model = pydantic.RootModel[overlay_Certificate]


# ===== overlay.CertificateId =====
class overlay_certificateId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb9\x60\xae\x8f"
    tl_type: typing.Literal["overlay.certificateId"] = pydantic.Field(
        alias="@type", default="overlay.certificateId"
    )
    overlay_id: Int256 = b"\x00" * 32
    node: Int256 = b"\x00" * 32
    expire_at: int = 0
    max_size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_certificateIdV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa7\xd2\x6c\xfc"
    tl_type: typing.Literal["overlay.certificateIdV2"] = pydantic.Field(
        alias="@type", default="overlay.certificateIdV2"
    )
    overlay_id: Int256 = b"\x00" * 32
    node: Int256 = b"\x00" * 32
    expire_at: int = 0
    max_size: int = 0
    flags: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_CertificateId = typing.Annotated[
    overlay_certificateId | overlay_certificateIdV2, pydantic.Field(discriminator="tl_type")
]
overlay_CertificateId_Model = pydantic.RootModel[overlay_CertificateId]


# ===== overlay.MemberCertificate =====
class overlay_memberCertificate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x59\x8c\x00\xc2"
    tl_type: typing.Literal["overlay.memberCertificate"] = pydantic.Field(
        alias="@type", default="overlay.memberCertificate"
    )
    issued_by: Option["PublicKey"] = None
    flags: int = 0
    slot: int = 0
    expire_at: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_emptyMemberCertificate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe2\x41\x24\xc0"
    tl_type: typing.Literal["overlay.emptyMemberCertificate"] = pydantic.Field(
        alias="@type", default="overlay.emptyMemberCertificate"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_MemberCertificate = typing.Annotated[
    overlay_memberCertificate | overlay_emptyMemberCertificate,
    pydantic.Field(discriminator="tl_type"),
]
overlay_MemberCertificate_Model = pydantic.RootModel[overlay_MemberCertificate]


# ===== overlay.MemberCertificateId =====
class overlay_memberCertificateId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x32\xdc\x87\x2c"
    tl_type: typing.Literal["overlay.memberCertificateId"] = pydantic.Field(
        alias="@type", default="overlay.memberCertificateId"
    )
    node: Option["adnl_id_Short"] = None
    flags: int = 0
    slot: int = 0
    expire_at: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_MemberCertificateId = overlay_memberCertificateId
overlay_MemberCertificateId_Model = pydantic.RootModel[overlay_MemberCertificateId]


# ===== overlay.Message =====
class overlay_message(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x20\x24\x25\x75"
    tl_type: typing.Literal["overlay.message"] = pydantic.Field(
        alias="@type", default="overlay.message"
    )
    overlay: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_messageWithExtra(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3d\x23\x32\xa2"
    tl_type: typing.Literal["overlay.messageWithExtra"] = pydantic.Field(
        alias="@type", default="overlay.messageWithExtra"
    )
    overlay: Int256 = b"\x00" * 32
    extra: Option["overlay_MessageExtra"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_Message = typing.Annotated[
    overlay_message | overlay_messageWithExtra, pydantic.Field(discriminator="tl_type")
]
overlay_Message_Model = pydantic.RootModel[overlay_Message]
type overlay_MessageExtra = None  # unsupported


# ===== overlay.Node =====
class overlay_node(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x83\x8a\x6b\xb8"
    tl_type: typing.Literal["overlay.node"] = pydantic.Field(alias="@type", default="overlay.node")
    id: Option["PublicKey"] = None
    overlay: Int256 = b"\x00" * 32
    version: int = 0
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_Node = overlay_node
overlay_Node_Model = pydantic.RootModel[overlay_Node]


# ===== overlay.NodeV2 =====
class overlay_nodeV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa7\xad\x9b\xbd"
    tl_type: typing.Literal["overlay.nodeV2"] = pydantic.Field(
        alias="@type", default="overlay.nodeV2"
    )
    id: Option["PublicKey"] = None
    overlay: Int256 = b"\x00" * 32
    flags: int = 0
    version: int = 0
    signature: pydantic.Base64Bytes = b""
    certificate: Option["overlay_MemberCertificate"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_NodeV2 = overlay_nodeV2
overlay_NodeV2_Model = pydantic.RootModel[overlay_NodeV2]


# ===== overlay.Nodes =====
class overlay_nodes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0e\x29\x87\xe4"
    tl_type: typing.Literal["overlay.nodes"] = pydantic.Field(
        alias="@type", default="overlay.nodes"
    )
    nodes: list["overlay_Node"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_Nodes = overlay_nodes
overlay_Nodes_Model = pydantic.RootModel[overlay_Nodes]


# ===== overlay.NodesV2 =====
class overlay_nodesV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x38\xe6\xd1\x1a"
    tl_type: typing.Literal["overlay.nodesV2"] = pydantic.Field(
        alias="@type", default="overlay.nodesV2"
    )
    nodes: list["overlay_NodeV2"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_NodesV2 = overlay_nodesV2
overlay_NodesV2_Model = pydantic.RootModel[overlay_NodesV2]


# ===== overlay.Pong =====
class overlay_pong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x04\x08\x70\x67"
    tl_type: typing.Literal["overlay.pong"] = pydantic.Field(alias="@type", default="overlay.pong")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_Pong = overlay_pong
overlay_Pong_Model = pydantic.RootModel[overlay_Pong]


# ===== overlay.broadcast.Id =====
class overlay_broadcast_id(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9a\x78\xfd\x51"
    tl_type: typing.Literal["overlay.broadcast.id"] = pydantic.Field(
        alias="@type", default="overlay.broadcast.id"
    )
    src: Int256 = b"\x00" * 32
    data_hash: Int256 = b"\x00" * 32
    flags: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_broadcast_Id = overlay_broadcast_id
overlay_broadcast_Id_Model = pydantic.RootModel[overlay_broadcast_Id]


# ===== overlay.broadcast.ToSign =====
class overlay_broadcast_toSign(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7c\x4e\x37\xfa"
    tl_type: typing.Literal["overlay.broadcast.toSign"] = pydantic.Field(
        alias="@type", default="overlay.broadcast.toSign"
    )
    hash: Int256 = b"\x00" * 32
    date: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_broadcast_ToSign = overlay_broadcast_toSign
overlay_broadcast_ToSign_Model = pydantic.RootModel[overlay_broadcast_ToSign]


# ===== overlay.broadcastFec.Id =====
class overlay_broadcastFec_id(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa6\x55\x31\xfb"
    tl_type: typing.Literal["overlay.broadcastFec.id"] = pydantic.Field(
        alias="@type", default="overlay.broadcastFec.id"
    )
    src: Int256 = b"\x00" * 32
    type: Int256 = b"\x00" * 32
    data_hash: Int256 = b"\x00" * 32
    size: int = 0
    flags: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_broadcastFec_Id = overlay_broadcastFec_id
overlay_broadcastFec_Id_Model = pydantic.RootModel[overlay_broadcastFec_Id]


# ===== overlay.broadcastFec.PartId =====
class overlay_broadcastFec_partId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd0\x62\x69\xa4"
    tl_type: typing.Literal["overlay.broadcastFec.partId"] = pydantic.Field(
        alias="@type", default="overlay.broadcastFec.partId"
    )
    broadcast_hash: Int256 = b"\x00" * 32
    data_hash: Int256 = b"\x00" * 32
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_broadcastFec_PartId = overlay_broadcastFec_partId
overlay_broadcastFec_PartId_Model = pydantic.RootModel[overlay_broadcastFec_PartId]


# ===== overlay.db.Key =====
class overlay_db_key_nodes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x16\x73\xd0\xc4"
    tl_type: typing.Literal["overlay.db.key.nodes"] = pydantic.Field(
        alias="@type", default="overlay.db.key.nodes"
    )
    local_id: Int256 = b"\x00" * 32
    overlay: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_db_Key = overlay_db_key_nodes
overlay_db_Key_Model = pydantic.RootModel[overlay_db_Key]


# ===== overlay.db.Nodes =====
class overlay_db_nodesV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x40\x21\xa9\x36"
    tl_type: typing.Literal["overlay.db.nodesV2"] = pydantic.Field(
        alias="@type", default="overlay.db.nodesV2"
    )
    nodes: Option["overlay_NodesV2"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_db_nodes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1a\xce\x88\xd5"
    tl_type: typing.Literal["overlay.db.nodes"] = pydantic.Field(
        alias="@type", default="overlay.db.nodes"
    )
    nodes: Option["overlay_Nodes"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_db_Nodes = typing.Annotated[
    overlay_db_nodesV2 | overlay_db_nodes, pydantic.Field(discriminator="tl_type")
]
overlay_db_Nodes_Model = pydantic.RootModel[overlay_db_Nodes]


# ===== overlay.node.ToSign =====
class overlay_node_toSign(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe1\xa8\xd8\x03"
    tl_type: typing.Literal["overlay.node.toSign"] = pydantic.Field(
        alias="@type", default="overlay.node.toSign"
    )
    id: Option["adnl_id_Short"] = None
    overlay: Int256 = b"\x00" * 32
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class overlay_node_toSignEx(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe9\x67\x7a\x84"
    tl_type: typing.Literal["overlay.node.toSignEx"] = pydantic.Field(
        alias="@type", default="overlay.node.toSignEx"
    )
    id: Option["adnl_id_Short"] = None
    overlay: Int256 = b"\x00" * 32
    flags: int = 0
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type overlay_node_ToSign = typing.Annotated[
    overlay_node_toSign | overlay_node_toSignEx, pydantic.Field(discriminator="tl_type")
]
overlay_node_ToSign_Model = pydantic.RootModel[overlay_node_ToSign]


# ===== proxyLiteserver.Config =====
class proxyLiteserver_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8e\x65\x31\x30"
    tl_type: typing.Literal["proxyLiteserver.config"] = pydantic.Field(
        alias="@type", default="proxyLiteserver.config"
    )
    port: int = 0
    id: Option["PublicKey"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type proxyLiteserver_Config = proxyLiteserver_config
proxyLiteserver_Config_Model = pydantic.RootModel[proxyLiteserver_Config]


# ===== rldp.Message =====
class rldp_message(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1e\xcd\x1b\x7d"
    tl_type: typing.Literal["rldp.message"] = pydantic.Field(alias="@type", default="rldp.message")
    id: Int256 = b"\x00" * 32
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rldp_query(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x69\x4d\x79\x8a"
    tl_type: typing.Literal["rldp.query"] = pydantic.Field(alias="@type", default="rldp.query")
    query_id: Int256 = b"\x00" * 32
    max_answer_size: int = 0
    timeout: int = 0
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rldp_answer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x03\x5c\xfc\xa3"
    tl_type: typing.Literal["rldp.answer"] = pydantic.Field(alias="@type", default="rldp.answer")
    query_id: Int256 = b"\x00" * 32
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type rldp_Message = typing.Annotated[
    rldp_message | rldp_query | rldp_answer, pydantic.Field(discriminator="tl_type")
]
rldp_Message_Model = pydantic.RootModel[rldp_Message]


# ===== rldp.MessagePart =====
class rldp_messagePart(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcc\x22\x5c\x18"
    tl_type: typing.Literal["rldp.messagePart"] = pydantic.Field(
        alias="@type", default="rldp.messagePart"
    )
    transfer_id: Int256 = b"\x00" * 32
    fec_type: Option["fec_Type"] = None
    part: int = 0
    total_size: int = 0
    seqno: int = 0
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rldp_confirm(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x58\xdc\x82\xf5"
    tl_type: typing.Literal["rldp.confirm"] = pydantic.Field(alias="@type", default="rldp.confirm")
    transfer_id: Int256 = b"\x00" * 32
    part: int = 0
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rldp_complete(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbf\xb2\x0c\xbc"
    tl_type: typing.Literal["rldp.complete"] = pydantic.Field(
        alias="@type", default="rldp.complete"
    )
    transfer_id: Int256 = b"\x00" * 32
    part: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type rldp_MessagePart = typing.Annotated[
    rldp_messagePart | rldp_confirm | rldp_complete, pydantic.Field(discriminator="tl_type")
]
rldp_MessagePart_Model = pydantic.RootModel[rldp_MessagePart]


# ===== rldp2.MessagePart =====
class rldp2_messagePart(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6e\x0b\x48\x11"
    tl_type: typing.Literal["rldp2.messagePart"] = pydantic.Field(
        alias="@type", default="rldp2.messagePart"
    )
    transfer_id: Int256 = b"\x00" * 32
    fec_type: Option["fec_Type"] = None
    part: int = 0
    total_size: int = 0
    seqno: int = 0
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rldp2_confirm(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x45\x99\xe6\x23"
    tl_type: typing.Literal["rldp2.confirm"] = pydantic.Field(
        alias="@type", default="rldp2.confirm"
    )
    transfer_id: Int256 = b"\x00" * 32
    part: int = 0
    max_seqno: int = 0
    received_mask: int = 0
    received_count: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rldp2_complete(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1f\x08\xb9\x36"
    tl_type: typing.Literal["rldp2.complete"] = pydantic.Field(
        alias="@type", default="rldp2.complete"
    )
    transfer_id: Int256 = b"\x00" * 32
    part: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type rldp2_MessagePart = typing.Annotated[
    rldp2_messagePart | rldp2_confirm | rldp2_complete, pydantic.Field(discriminator="tl_type")
]
rldp2_MessagePart_Model = pydantic.RootModel[rldp2_MessagePart]


# ===== storage.Piece =====
class storage_piece(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0d\xfa\xb4\x80"
    tl_type: typing.Literal["storage.piece"] = pydantic.Field(
        alias="@type", default="storage.piece"
    )
    proof: pydantic.Base64Bytes = b""
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_Piece = storage_piece
storage_Piece_Model = pydantic.RootModel[storage_Piece]


# ===== storage.Pong =====
class storage_pong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa5\xc6\xf5\x6c"
    tl_type: typing.Literal["storage.pong"] = pydantic.Field(alias="@type", default="storage.pong")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_Pong = storage_pong
storage_Pong_Model = pydantic.RootModel[storage_Pong]


# ===== storage.PriorityAction =====
class storage_priorityAction_all(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x40\x89\x23\xfe"
    tl_type: typing.Literal["storage.priorityAction.all"] = pydantic.Field(
        alias="@type", default="storage.priorityAction.all"
    )
    priority: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class storage_priorityAction_idx(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x28\xb7\x0f\x95"
    tl_type: typing.Literal["storage.priorityAction.idx"] = pydantic.Field(
        alias="@type", default="storage.priorityAction.idx"
    )
    idx: int = 0
    priority: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class storage_priorityAction_name(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc0\xd1\x24\x01"
    tl_type: typing.Literal["storage.priorityAction.name"] = pydantic.Field(
        alias="@type", default="storage.priorityAction.name"
    )
    name: str = ""
    priority: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_PriorityAction = typing.Annotated[
    storage_priorityAction_all | storage_priorityAction_idx | storage_priorityAction_name,
    pydantic.Field(discriminator="tl_type"),
]
storage_PriorityAction_Model = pydantic.RootModel[storage_PriorityAction]


# ===== storage.State =====
class storage_state(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8a\x70\x13\x33"
    tl_type: typing.Literal["storage.state"] = pydantic.Field(
        alias="@type", default="storage.state"
    )
    will_upload: bool = False
    want_download: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_State = storage_state
storage_State_Model = pydantic.RootModel[storage_State]


# ===== storage.TorrentInfo =====
class storage_torrentInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xee\xd0\xce\x14"
    tl_type: typing.Literal["storage.torrentInfo"] = pydantic.Field(
        alias="@type", default="storage.torrentInfo"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_TorrentInfo = storage_torrentInfo
storage_TorrentInfo_Model = pydantic.RootModel[storage_TorrentInfo]


# ===== storage.Update =====
class storage_updateInit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb6\xe0\x33\xce"
    tl_type: typing.Literal["storage.updateInit"] = pydantic.Field(
        alias="@type", default="storage.updateInit"
    )
    have_pieces: pydantic.Base64Bytes = b""
    have_pieces_offset: int = 0
    state: Option["storage_State"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class storage_updateHavePieces(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x49\x20\xf8\x3b"
    tl_type: typing.Literal["storage.updateHavePieces"] = pydantic.Field(
        alias="@type", default="storage.updateHavePieces"
    )
    piece_id: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class storage_updateState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb5\x34\xb0\x05"
    tl_type: typing.Literal["storage.updateState"] = pydantic.Field(
        alias="@type", default="storage.updateState"
    )
    state: Option["storage_State"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_Update = typing.Annotated[
    storage_updateInit | storage_updateHavePieces | storage_updateState,
    pydantic.Field(discriminator="tl_type"),
]
storage_Update_Model = pydantic.RootModel[storage_Update]


# ===== storage.daemon.ContractInfo =====
class storage_daemon_contractInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x09\x4f\x74\xd7"
    tl_type: typing.Literal["storage.daemon.contractInfo"] = pydantic.Field(
        alias="@type", default="storage.daemon.contractInfo"
    )
    address: str = ""
    state: int = 0
    torrent: Int256 = b"\x00" * 32
    created_time: int = 0
    file_size: int = 0
    downloaded_size: int = 0
    rate: str = ""
    max_span: int = 0
    client_balance: str = ""
    contract_balance: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_ContractInfo = storage_daemon_contractInfo
storage_daemon_ContractInfo_Model = pydantic.RootModel[storage_daemon_ContractInfo]
type storage_daemon_FileInfo = None  # unsupported


# ===== storage.daemon.FilePiecesInfo =====
class storage_daemon_filePiecesInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x43\x35\x55\x03"
    tl_type: typing.Literal["storage.daemon.filePiecesInfo"] = pydantic.Field(
        alias="@type", default="storage.daemon.filePiecesInfo"
    )
    name: str = ""
    range_l: int = 0
    range_r: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_FilePiecesInfo = storage_daemon_filePiecesInfo
storage_daemon_FilePiecesInfo_Model = pydantic.RootModel[storage_daemon_FilePiecesInfo]


# ===== storage.daemon.KeyHash =====
class storage_daemon_keyHash(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xdc\x62\xb5\x85"
    tl_type: typing.Literal["storage.daemon.keyHash"] = pydantic.Field(
        alias="@type", default="storage.daemon.keyHash"
    )
    key_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_KeyHash = storage_daemon_keyHash
storage_daemon_KeyHash_Model = pydantic.RootModel[storage_daemon_KeyHash]


# ===== storage.daemon.NewContractMessage =====
class storage_daemon_newContractMessage(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf6\xad\x89\xf5"
    tl_type: typing.Literal["storage.daemon.newContractMessage"] = pydantic.Field(
        alias="@type", default="storage.daemon.newContractMessage"
    )
    body: pydantic.Base64Bytes = b""
    rate: str = ""
    max_span: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_NewContractMessage = storage_daemon_newContractMessage
storage_daemon_NewContractMessage_Model = pydantic.RootModel[storage_daemon_NewContractMessage]


# ===== storage.daemon.NewContractParams =====
class storage_daemon_newContractParams(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\x08\x5b\x55"
    tl_type: typing.Literal["storage.daemon.newContractParams"] = pydantic.Field(
        alias="@type", default="storage.daemon.newContractParams"
    )
    rate: str = ""
    max_span: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class storage_daemon_newContractParamsAuto(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xad\x67\x77\xaf"
    tl_type: typing.Literal["storage.daemon.newContractParamsAuto"] = pydantic.Field(
        alias="@type", default="storage.daemon.newContractParamsAuto"
    )
    provider_address: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_NewContractParams = typing.Annotated[
    storage_daemon_newContractParams | storage_daemon_newContractParamsAuto,
    pydantic.Field(discriminator="tl_type"),
]
storage_daemon_NewContractParams_Model = pydantic.RootModel[storage_daemon_NewContractParams]


# ===== storage.daemon.Peer =====
class storage_daemon_peer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x34\x50\x34\xbd"
    tl_type: typing.Literal["storage.daemon.peer"] = pydantic.Field(
        alias="@type", default="storage.daemon.peer"
    )
    adnl_id: Int256 = b"\x00" * 32
    ip_str: str = ""
    download_speed: float = 0
    upload_speed: float = 0
    ready_parts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_Peer = storage_daemon_peer
storage_daemon_Peer_Model = pydantic.RootModel[storage_daemon_Peer]


# ===== storage.daemon.PeerList =====
class storage_daemon_peerList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x15\x38\xde\xa5"
    tl_type: typing.Literal["storage.daemon.peerList"] = pydantic.Field(
        alias="@type", default="storage.daemon.peerList"
    )
    peers: list["storage_daemon_Peer"] = []
    download_speed: float = 0
    upload_speed: float = 0
    total_parts: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_PeerList = storage_daemon_peerList
storage_daemon_PeerList_Model = pydantic.RootModel[storage_daemon_PeerList]


# ===== storage.daemon.ProviderAddress =====
class storage_daemon_providerAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x12\xb9\x5e\x88"
    tl_type: typing.Literal["storage.daemon.providerAddress"] = pydantic.Field(
        alias="@type", default="storage.daemon.providerAddress"
    )
    address: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_ProviderAddress = storage_daemon_providerAddress
storage_daemon_ProviderAddress_Model = pydantic.RootModel[storage_daemon_ProviderAddress]


# ===== storage.daemon.ProviderConfig =====
class storage_daemon_providerConfig(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x94\x0a\xad\x7d"
    tl_type: typing.Literal["storage.daemon.providerConfig"] = pydantic.Field(
        alias="@type", default="storage.daemon.providerConfig"
    )
    max_contracts: int = 0
    max_total_size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_ProviderConfig = storage_daemon_providerConfig
storage_daemon_ProviderConfig_Model = pydantic.RootModel[storage_daemon_ProviderConfig]


# ===== storage.daemon.ProviderInfo =====
class storage_daemon_providerInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2d\x01\x6b\xe7"
    tl_type: typing.Literal["storage.daemon.providerInfo"] = pydantic.Field(
        alias="@type", default="storage.daemon.providerInfo"
    )
    address: str = ""
    balance: str = ""
    config: Option["storage_daemon_ProviderConfig"] = None
    contracts_count: int = 0
    contracts_total_size: int = 0
    contracts: list["storage_daemon_ContractInfo"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_ProviderInfo = storage_daemon_providerInfo
storage_daemon_ProviderInfo_Model = pydantic.RootModel[storage_daemon_ProviderInfo]


# ===== storage.daemon.QueryError =====
class storage_daemon_queryError(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc4\xba\xbd\x04"
    tl_type: typing.Literal["storage.daemon.queryError"] = pydantic.Field(
        alias="@type", default="storage.daemon.queryError"
    )
    message: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_QueryError = storage_daemon_queryError
storage_daemon_QueryError_Model = pydantic.RootModel[storage_daemon_QueryError]


# ===== storage.daemon.SetPriorityStatus =====
class storage_daemon_prioritySet(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd7\x9f\xe8\xb6"
    tl_type: typing.Literal["storage.daemon.prioritySet"] = pydantic.Field(
        alias="@type", default="storage.daemon.prioritySet"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class storage_daemon_priorityPending(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa6\x61\x09\x84"
    tl_type: typing.Literal["storage.daemon.priorityPending"] = pydantic.Field(
        alias="@type", default="storage.daemon.priorityPending"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_SetPriorityStatus = typing.Annotated[
    storage_daemon_prioritySet | storage_daemon_priorityPending,
    pydantic.Field(discriminator="tl_type"),
]
storage_daemon_SetPriorityStatus_Model = pydantic.RootModel[storage_daemon_SetPriorityStatus]


# ===== storage.daemon.SpeedLimits =====
class storage_daemon_speedLimits(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x19\xe9\xb0\xfe"
    tl_type: typing.Literal["storage.daemon.speedLimits"] = pydantic.Field(
        alias="@type", default="storage.daemon.speedLimits"
    )
    download: float = 0
    upload: float = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_SpeedLimits = storage_daemon_speedLimits
storage_daemon_SpeedLimits_Model = pydantic.RootModel[storage_daemon_SpeedLimits]


# ===== storage.daemon.Success =====
class storage_daemon_success(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1c\xef\xae\xb3"
    tl_type: typing.Literal["storage.daemon.success"] = pydantic.Field(
        alias="@type", default="storage.daemon.success"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_Success = storage_daemon_success
storage_daemon_Success_Model = pydantic.RootModel[storage_daemon_Success]
type storage_daemon_Torrent = None  # unsupported


# ===== storage.daemon.TorrentFull =====
class storage_daemon_torrentFull(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x27\x8c\xa8\x5f"
    tl_type: typing.Literal["storage.daemon.torrentFull"] = pydantic.Field(
        alias="@type", default="storage.daemon.torrentFull"
    )
    torrent: Option["storage_daemon_Torrent"] = None
    files: list["storage_daemon_FileInfo"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_TorrentFull = storage_daemon_torrentFull
storage_daemon_TorrentFull_Model = pydantic.RootModel[storage_daemon_TorrentFull]


# ===== storage.daemon.TorrentList =====
class storage_daemon_torrentList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x42\x18\x1c\x4f"
    tl_type: typing.Literal["storage.daemon.torrentList"] = pydantic.Field(
        alias="@type", default="storage.daemon.torrentList"
    )
    torrents: list["storage_daemon_Torrent"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_TorrentList = storage_daemon_torrentList
storage_daemon_TorrentList_Model = pydantic.RootModel[storage_daemon_TorrentList]


# ===== storage.daemon.TorrentMeta =====
class storage_daemon_torrentMeta(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe8\x1e\xbe\xd4"
    tl_type: typing.Literal["storage.daemon.torrentMeta"] = pydantic.Field(
        alias="@type", default="storage.daemon.torrentMeta"
    )
    meta: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_TorrentMeta = storage_daemon_torrentMeta
storage_daemon_TorrentMeta_Model = pydantic.RootModel[storage_daemon_TorrentMeta]
type storage_daemon_TorrentPiecesInfo = None  # unsupported


# ===== storage.daemon.provider.Config =====
class storage_daemon_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb7\x94\xc6\xf0"
    tl_type: typing.Literal["storage.daemon.config"] = pydantic.Field(
        alias="@type", default="storage.daemon.config"
    )
    server_key: Option["PublicKey"] = None
    cli_key_hash: Int256 = b"\x00" * 32
    provider_address: str = ""
    adnl_id: Option["PublicKey"] = None
    dht_id: Option["PublicKey"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_provider_Config = storage_daemon_config
storage_daemon_provider_Config_Model = pydantic.RootModel[storage_daemon_provider_Config]


# ===== storage.daemon.provider.Params =====
class storage_daemon_provider_params(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x07\x6e\x73\xac"
    tl_type: typing.Literal["storage.daemon.provider.params"] = pydantic.Field(
        alias="@type", default="storage.daemon.provider.params"
    )
    accept_new_contracts: bool = False
    rate_per_mb_day: str = ""
    max_span: int = 0
    minimal_file_size: int = 0
    maximal_file_size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_daemon_provider_Params = storage_daemon_provider_params
storage_daemon_provider_Params_Model = pydantic.RootModel[storage_daemon_provider_Params]
type storage_db_Config = None  # unsupported


# ===== storage.db.ContractAddress =====
class storage_provider_db_contractAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfd\xec\x58\xe2"
    tl_type: typing.Literal["storage.provider.db.contractAddress"] = pydantic.Field(
        alias="@type", default="storage.provider.db.contractAddress"
    )
    wc: int = 0
    addr: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_ContractAddress = storage_provider_db_contractAddress
storage_db_ContractAddress_Model = pydantic.RootModel[storage_db_ContractAddress]


# ===== storage.db.ContractList =====
class storage_provider_db_contractList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x17\xe7\x38\xda"
    tl_type: typing.Literal["storage.provider.db.contractList"] = pydantic.Field(
        alias="@type", default="storage.provider.db.contractList"
    )
    contracts: list["storage_db_ContractAddress"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_ContractList = storage_provider_db_contractList
storage_db_ContractList_Model = pydantic.RootModel[storage_db_ContractList]


# ===== storage.db.PiecesInDb =====
class storage_db_piecesInDb(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3c\xb4\x19\x06"
    tl_type: typing.Literal["storage.db.piecesInDb"] = pydantic.Field(
        alias="@type", default="storage.db.piecesInDb"
    )
    pieces: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_PiecesInDb = storage_db_piecesInDb
storage_db_PiecesInDb_Model = pydantic.RootModel[storage_db_PiecesInDb]


# ===== storage.db.Priorities =====
class storage_db_priorities(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4e\xeb\x29\x39"
    tl_type: typing.Literal["storage.db.priorities"] = pydantic.Field(
        alias="@type", default="storage.db.priorities"
    )
    actions: list["storage_PriorityAction"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_Priorities = storage_db_priorities
storage_db_Priorities_Model = pydantic.RootModel[storage_db_Priorities]


# ===== storage.db.TorrentList =====
class storage_db_torrentList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x81\xe3\xef\x59"
    tl_type: typing.Literal["storage.db.torrentList"] = pydantic.Field(
        alias="@type", default="storage.db.torrentList"
    )
    torrents: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_TorrentList = storage_db_torrentList
storage_db_TorrentList_Model = pydantic.RootModel[storage_db_TorrentList]


# ===== storage.db.TorrentShort =====
class storage_db_torrent(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9a\x37\x2d\xad"
    tl_type: typing.Literal["storage.db.torrent"] = pydantic.Field(
        alias="@type", default="storage.db.torrent"
    )
    root_dir: str = ""
    active_download: bool = False
    active_upload: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_TorrentShort = storage_db_torrent
storage_db_TorrentShort_Model = pydantic.RootModel[storage_db_TorrentShort]


# ===== storage.db.key.Config =====
class storage_db_key_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x07\x34\x98\xdd"
    tl_type: typing.Literal["storage.db.key.config"] = pydantic.Field(
        alias="@type", default="storage.db.key.config"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_Config = storage_db_key_config
storage_db_key_Config_Model = pydantic.RootModel[storage_db_key_Config]


# ===== storage.db.key.PieceInDb =====
class storage_db_key_pieceInDb(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcd\xed\x0a\xc4"
    tl_type: typing.Literal["storage.db.key.pieceInDb"] = pydantic.Field(
        alias="@type", default="storage.db.key.pieceInDb"
    )
    hash: Int256 = b"\x00" * 32
    idx: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_PieceInDb = storage_db_key_pieceInDb
storage_db_key_PieceInDb_Model = pydantic.RootModel[storage_db_key_PieceInDb]


# ===== storage.db.key.PiecesInDb =====
class storage_db_key_piecesInDb(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe3\xc9\xe8\xdb"
    tl_type: typing.Literal["storage.db.key.piecesInDb"] = pydantic.Field(
        alias="@type", default="storage.db.key.piecesInDb"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_PiecesInDb = storage_db_key_piecesInDb
storage_db_key_PiecesInDb_Model = pydantic.RootModel[storage_db_key_PiecesInDb]


# ===== storage.db.key.Priorities =====
class storage_db_key_priorities(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6d\xca\xf1\xb5"
    tl_type: typing.Literal["storage.db.key.priorities"] = pydantic.Field(
        alias="@type", default="storage.db.key.priorities"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_Priorities = storage_db_key_priorities
storage_db_key_Priorities_Model = pydantic.RootModel[storage_db_key_Priorities]


# ===== storage.db.key.TorrentList =====
class storage_db_key_torrentList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x56\xe8\xc6\xcb"
    tl_type: typing.Literal["storage.db.key.torrentList"] = pydantic.Field(
        alias="@type", default="storage.db.key.torrentList"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_TorrentList = storage_db_key_torrentList
storage_db_key_TorrentList_Model = pydantic.RootModel[storage_db_key_TorrentList]


# ===== storage.db.key.TorrentMeta =====
class storage_db_key_torrentMeta(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x66\x9d\x23\x62"
    tl_type: typing.Literal["storage.db.key.torrentMeta"] = pydantic.Field(
        alias="@type", default="storage.db.key.torrentMeta"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_TorrentMeta = storage_db_key_torrentMeta
storage_db_key_TorrentMeta_Model = pydantic.RootModel[storage_db_key_TorrentMeta]


# ===== storage.db.key.TorrentShort =====
class storage_db_key_torrent(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2f\x12\x88\xb9"
    tl_type: typing.Literal["storage.db.key.torrent"] = pydantic.Field(
        alias="@type", default="storage.db.key.torrent"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_db_key_TorrentShort = storage_db_key_torrent
storage_db_key_TorrentShort_Model = pydantic.RootModel[storage_db_key_TorrentShort]


# ===== storage.provider.db.MicrochunkTree =====
class storage_provider_db_microchunkTree(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x42\x0f\xca\xc2"
    tl_type: typing.Literal["storage.provider.db.microchunkTree"] = pydantic.Field(
        alias="@type", default="storage.provider.db.microchunkTree"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_MicrochunkTree = storage_provider_db_microchunkTree
storage_provider_db_MicrochunkTree_Model = pydantic.RootModel[storage_provider_db_MicrochunkTree]


# ===== storage.provider.db.State =====
class storage_provider_db_state(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf3\x55\xa9\x01"
    tl_type: typing.Literal["storage.provider.db.state"] = pydantic.Field(
        alias="@type", default="storage.provider.db.state"
    )
    last_processed_lt: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_State = storage_provider_db_state
storage_provider_db_State_Model = pydantic.RootModel[storage_provider_db_State]


# ===== storage.provider.db.StorageContract =====
class storage_provider_db_storageContract(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x32\xa7\xb3\xee"
    tl_type: typing.Literal["storage.provider.db.storageContract"] = pydantic.Field(
        alias="@type", default="storage.provider.db.storageContract"
    )
    torrent_hash: Int256 = b"\x00" * 32
    microchunk_hash: Int256 = b"\x00" * 32
    created_time: int = 0
    state: int = 0
    file_size: int = 0
    rate: str = ""
    max_span: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_StorageContract = storage_provider_db_storageContract
storage_provider_db_StorageContract_Model = pydantic.RootModel[storage_provider_db_StorageContract]


# ===== storage.provider.db.key.ContractList =====
class storage_provider_db_key_contractList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x46\xcc\x92\x55"
    tl_type: typing.Literal["storage.provider.db.key.contractList"] = pydantic.Field(
        alias="@type", default="storage.provider.db.key.contractList"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_key_ContractList = storage_provider_db_key_contractList
storage_provider_db_key_ContractList_Model = pydantic.RootModel[
    storage_provider_db_key_ContractList
]


# ===== storage.provider.db.key.MicrochunkTree =====
class storage_provider_db_key_microchunkTree(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x79\xea\x98\x29"
    tl_type: typing.Literal["storage.provider.db.key.microchunkTree"] = pydantic.Field(
        alias="@type", default="storage.provider.db.key.microchunkTree"
    )
    wc: int = 0
    addr: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_key_MicrochunkTree = storage_provider_db_key_microchunkTree
storage_provider_db_key_MicrochunkTree_Model = pydantic.RootModel[
    storage_provider_db_key_MicrochunkTree
]


# ===== storage.provider.db.key.ProviderConfig =====
class storage_provider_db_key_providerConfig(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x32\x09\xe8\xe7"
    tl_type: typing.Literal["storage.provider.db.key.providerConfig"] = pydantic.Field(
        alias="@type", default="storage.provider.db.key.providerConfig"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_key_ProviderConfig = storage_provider_db_key_providerConfig
storage_provider_db_key_ProviderConfig_Model = pydantic.RootModel[
    storage_provider_db_key_ProviderConfig
]


# ===== storage.provider.db.key.State =====
class storage_provider_db_key_state(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa2\xcf\x20\xf4"
    tl_type: typing.Literal["storage.provider.db.key.state"] = pydantic.Field(
        alias="@type", default="storage.provider.db.key.state"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_key_State = storage_provider_db_key_state
storage_provider_db_key_State_Model = pydantic.RootModel[storage_provider_db_key_State]


# ===== storage.provider.db.key.StorageContract =====
class storage_provider_db_key_storageContract(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1e\xcb\x5e\xcc"
    tl_type: typing.Literal["storage.provider.db.key.storageContract"] = pydantic.Field(
        alias="@type", default="storage.provider.db.key.storageContract"
    )
    wc: int = 0
    addr: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type storage_provider_db_key_StorageContract = storage_provider_db_key_storageContract
storage_provider_db_key_StorageContract_Model = pydantic.RootModel[
    storage_provider_db_key_StorageContract
]


# ===== tcp.Message =====
class tcp_authentificate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x12\xab\x5b\x44"
    tl_type: typing.Literal["tcp.authentificate"] = pydantic.Field(
        alias="@type", default="tcp.authentificate"
    )
    nonce: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tcp_authentificationNonce(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb6\x4a\x5d\xe3"
    tl_type: typing.Literal["tcp.authentificationNonce"] = pydantic.Field(
        alias="@type", default="tcp.authentificationNonce"
    )
    nonce: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tcp_authentificationComplete(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa6\x9e\xad\xf7"
    tl_type: typing.Literal["tcp.authentificationComplete"] = pydantic.Field(
        alias="@type", default="tcp.authentificationComplete"
    )
    key: Option["PublicKey"] = None
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tcp_Message = typing.Annotated[
    tcp_authentificate | tcp_authentificationNonce | tcp_authentificationComplete,
    pydantic.Field(discriminator="tl_type"),
]
tcp_Message_Model = pydantic.RootModel[tcp_Message]


# ===== tcp.Pong =====
class tcp_pong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x03\xfb\x69\xdc"
    tl_type: typing.Literal["tcp.pong"] = pydantic.Field(alias="@type", default="tcp.pong")
    random_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tcp_Pong = tcp_pong
tcp_Pong_Model = pydantic.RootModel[tcp_Pong]


# ===== ton.BlockId =====
class ton_blockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x70\x6e\x0b\xc5"
    tl_type: typing.Literal["ton.blockId"] = pydantic.Field(alias="@type", default="ton.blockId")
    root_cell_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class ton_blockIdApprove(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x49\x4a\xd4\x2d"
    tl_type: typing.Literal["ton.blockIdApprove"] = pydantic.Field(
        alias="@type", default="ton.blockIdApprove"
    )
    root_cell_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ton_BlockId = typing.Annotated[
    ton_blockId | ton_blockIdApprove, pydantic.Field(discriminator="tl_type")
]
ton_BlockId_Model = pydantic.RootModel[ton_BlockId]


# ===== tonNode.ArchiveInfo =====
class tonNode_archiveNotFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x83\x16\x29\x99"
    tl_type: typing.Literal["tonNode.archiveNotFound"] = pydantic.Field(
        alias="@type", default="tonNode.archiveNotFound"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_archiveInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8c\xff\xef\x19"
    tl_type: typing.Literal["tonNode.archiveInfo"] = pydantic.Field(
        alias="@type", default="tonNode.archiveInfo"
    )
    id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_ArchiveInfo = typing.Annotated[
    tonNode_archiveNotFound | tonNode_archiveInfo, pydantic.Field(discriminator="tl_type")
]
tonNode_ArchiveInfo_Model = pydantic.RootModel[tonNode_ArchiveInfo]


# ===== tonNode.BlockDescription =====
class tonNode_blockDescriptionEmpty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x95\xae\x84\x83"
    tl_type: typing.Literal["tonNode.blockDescriptionEmpty"] = pydantic.Field(
        alias="@type", default="tonNode.blockDescriptionEmpty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_blockDescription(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x88\xd0\xa1\x46"
    tl_type: typing.Literal["tonNode.blockDescription"] = pydantic.Field(
        alias="@type", default="tonNode.blockDescription"
    )
    id: Option["tonNode_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_BlockDescription = typing.Annotated[
    tonNode_blockDescriptionEmpty | tonNode_blockDescription,
    pydantic.Field(discriminator="tl_type"),
]
tonNode_BlockDescription_Model = pydantic.RootModel[tonNode_BlockDescription]


# ===== tonNode.BlockId =====
class tonNode_blockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x67\xb1\xcd\xb7"
    tl_type: typing.Literal["tonNode.blockId"] = pydantic.Field(
        alias="@type", default="tonNode.blockId"
    )
    workchain: int = 0
    shard: int = 0
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_BlockId = tonNode_blockId
tonNode_BlockId_Model = pydantic.RootModel[tonNode_BlockId]


# ===== tonNode.BlockIdExt =====
class tonNode_blockIdExt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x78\xeb\x52\x67"
    tl_type: typing.Literal["tonNode.blockIdExt"] = pydantic.Field(
        alias="@type", default="tonNode.blockIdExt"
    )
    workchain: int = 0
    shard: int = 0
    seqno: int = 0
    root_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_BlockIdExt = tonNode_blockIdExt
tonNode_BlockIdExt_Model = pydantic.RootModel[tonNode_BlockIdExt]


# ===== tonNode.BlockSignature =====
class tonNode_blockSignature(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x33\x3c\xf0\x50"
    tl_type: typing.Literal["tonNode.blockSignature"] = pydantic.Field(
        alias="@type", default="tonNode.blockSignature"
    )
    who: Int256 = b"\x00" * 32
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_BlockSignature = tonNode_blockSignature
tonNode_BlockSignature_Model = pydantic.RootModel[tonNode_BlockSignature]


# ===== tonNode.BlocksDescription =====
class tonNode_blocksDescription(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2c\x61\x2a\xd6"
    tl_type: typing.Literal["tonNode.blocksDescription"] = pydantic.Field(
        alias="@type", default="tonNode.blocksDescription"
    )
    ids: list["tonNode_BlockIdExt"] = []
    incomplete: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_BlocksDescription = tonNode_blocksDescription
tonNode_BlocksDescription_Model = pydantic.RootModel[tonNode_BlocksDescription]


# ===== tonNode.Broadcast =====
class tonNode_blockBroadcast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x05\x11\x2e\xae"
    tl_type: typing.Literal["tonNode.blockBroadcast"] = pydantic.Field(
        alias="@type", default="tonNode.blockBroadcast"
    )
    id: Option["tonNode_BlockIdExt"] = None
    catchain_seqno: int = 0
    validator_set_hash: int = 0
    signatures: list["tonNode_BlockSignature"] = []
    proof: pydantic.Base64Bytes = b""
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_ihrMessageBroadcast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb3\xa4\x5d\x52"
    tl_type: typing.Literal["tonNode.ihrMessageBroadcast"] = pydantic.Field(
        alias="@type", default="tonNode.ihrMessageBroadcast"
    )
    message: Option["tonNode_IhrMessage"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_externalMessageBroadcast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x67\x18\x1b\x3d"
    tl_type: typing.Literal["tonNode.externalMessageBroadcast"] = pydantic.Field(
        alias="@type", default="tonNode.externalMessageBroadcast"
    )
    message: Option["tonNode_ExternalMessage"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_newShardBlockBroadcast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbc\xfa\xf2\x0a"
    tl_type: typing.Literal["tonNode.newShardBlockBroadcast"] = pydantic.Field(
        alias="@type", default="tonNode.newShardBlockBroadcast"
    )
    block: Option["tonNode_NewShardBlock"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_newBlockCandidateBroadcast(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc0\x0d\xed\xbf"
    tl_type: typing.Literal["tonNode.newBlockCandidateBroadcast"] = pydantic.Field(
        alias="@type", default="tonNode.newBlockCandidateBroadcast"
    )
    id: Option["tonNode_BlockIdExt"] = None
    catchain_seqno: int = 0
    validator_set_hash: int = 0
    collator_signature: Option["tonNode_BlockSignature"] = None
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_Broadcast = typing.Annotated[
    tonNode_blockBroadcast
    | tonNode_ihrMessageBroadcast
    | tonNode_externalMessageBroadcast
    | tonNode_newShardBlockBroadcast
    | tonNode_newBlockCandidateBroadcast,
    pydantic.Field(discriminator="tl_type"),
]
tonNode_Broadcast_Model = pydantic.RootModel[tonNode_Broadcast]
type tonNode_Capabilities = None  # unsupported


# ===== tonNode.CustomOverlayId =====
class tonNode_customOverlayId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa6\x1e\xb4\x39"
    tl_type: typing.Literal["tonNode.customOverlayId"] = pydantic.Field(
        alias="@type", default="tonNode.customOverlayId"
    )
    zero_state_file_hash: Int256 = b"\x00" * 32
    name: str = ""
    nodes: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_CustomOverlayId = tonNode_customOverlayId
tonNode_CustomOverlayId_Model = pydantic.RootModel[tonNode_CustomOverlayId]


# ===== tonNode.Data =====
class tonNode_data(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\x24\x0a\x56"
    tl_type: typing.Literal["tonNode.data"] = pydantic.Field(alias="@type", default="tonNode.data")
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_Data = tonNode_data
tonNode_Data_Model = pydantic.RootModel[tonNode_Data]


# ===== tonNode.DataFull =====
class tonNode_dataFull(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x93\x9f\x58\xbe"
    tl_type: typing.Literal["tonNode.dataFull"] = pydantic.Field(
        alias="@type", default="tonNode.dataFull"
    )
    id: Option["tonNode_BlockIdExt"] = None
    proof: pydantic.Base64Bytes = b""
    block: pydantic.Base64Bytes = b""
    is_link: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_dataFullEmpty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xca\x85\x6e\x57"
    tl_type: typing.Literal["tonNode.dataFullEmpty"] = pydantic.Field(
        alias="@type", default="tonNode.dataFullEmpty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_DataFull = typing.Annotated[
    tonNode_dataFull | tonNode_dataFullEmpty, pydantic.Field(discriminator="tl_type")
]
tonNode_DataFull_Model = pydantic.RootModel[tonNode_DataFull]


# ===== tonNode.ExternalMessage =====
class tonNode_externalMessage(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x09\xa2\x75\xdc"
    tl_type: typing.Literal["tonNode.externalMessage"] = pydantic.Field(
        alias="@type", default="tonNode.externalMessage"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_ExternalMessage = tonNode_externalMessage
tonNode_ExternalMessage_Model = pydantic.RootModel[tonNode_ExternalMessage]


# ===== tonNode.ForgetPeer =====
class tonNode_forgetPeer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x81\xf4\x22\xab"
    tl_type: typing.Literal["tonNode.forgetPeer"] = pydantic.Field(
        alias="@type", default="tonNode.forgetPeer"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_ForgetPeer = tonNode_forgetPeer
tonNode_ForgetPeer_Model = pydantic.RootModel[tonNode_ForgetPeer]


# ===== tonNode.IhrMessage =====
class tonNode_ihrMessage(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x07\xc3\x34\x45"
    tl_type: typing.Literal["tonNode.ihrMessage"] = pydantic.Field(
        alias="@type", default="tonNode.ihrMessage"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_IhrMessage = tonNode_ihrMessage
tonNode_IhrMessage_Model = pydantic.RootModel[tonNode_IhrMessage]


# ===== tonNode.KeyBlocks =====
class tonNode_keyBlocks(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x59\x4d\x66\x07"
    tl_type: typing.Literal["tonNode.keyBlocks"] = pydantic.Field(
        alias="@type", default="tonNode.keyBlocks"
    )
    blocks: list["tonNode_BlockIdExt"] = []
    incomplete: bool = False
    error: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_KeyBlocks = tonNode_keyBlocks
tonNode_KeyBlocks_Model = pydantic.RootModel[tonNode_KeyBlocks]


# ===== tonNode.NewShardBlock =====
class tonNode_newShardBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x29\xc2\x9d\xa4"
    tl_type: typing.Literal["tonNode.newShardBlock"] = pydantic.Field(
        alias="@type", default="tonNode.newShardBlock"
    )
    block: Option["tonNode_BlockIdExt"] = None
    cc_seqno: int = 0
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_NewShardBlock = tonNode_newShardBlock
tonNode_NewShardBlock_Model = pydantic.RootModel[tonNode_NewShardBlock]


# ===== tonNode.OutMsgQueueProof =====
class tonNode_outMsgQueueProof(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd2\x1b\x02\x5c"
    tl_type: typing.Literal["tonNode.outMsgQueueProof"] = pydantic.Field(
        alias="@type", default="tonNode.outMsgQueueProof"
    )
    queue_proofs: pydantic.Base64Bytes = b""
    block_state_proofs: pydantic.Base64Bytes = b""
    msg_counts: list[int] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_outMsgQueueProofEmpty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe2\x0e\x9a\xa0"
    tl_type: typing.Literal["tonNode.outMsgQueueProofEmpty"] = pydantic.Field(
        alias="@type", default="tonNode.outMsgQueueProofEmpty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_OutMsgQueueProof = typing.Annotated[
    tonNode_outMsgQueueProof | tonNode_outMsgQueueProofEmpty,
    pydantic.Field(discriminator="tl_type"),
]
tonNode_OutMsgQueueProof_Model = pydantic.RootModel[tonNode_OutMsgQueueProof]


# ===== tonNode.PersistentStateIdV2 =====
class tonNode_persistentStateIdV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x63\x50\xec\x09"
    tl_type: typing.Literal["tonNode.persistentStateIdV2"] = pydantic.Field(
        alias="@type", default="tonNode.persistentStateIdV2"
    )
    block: Option["tonNode_BlockIdExt"] = None
    masterchain_block: Option["tonNode_BlockIdExt"] = None
    effective_shard: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_PersistentStateIdV2 = tonNode_persistentStateIdV2
tonNode_PersistentStateIdV2_Model = pydantic.RootModel[tonNode_PersistentStateIdV2]


# ===== tonNode.PersistentStateSize =====
class tonNode_persistentStateSize(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd2\xd3\x24\xf2"
    tl_type: typing.Literal["tonNode.persistentStateSize"] = pydantic.Field(
        alias="@type", default="tonNode.persistentStateSize"
    )
    size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_persistentStateSizeNotFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6b\xc0\x56\xfa"
    tl_type: typing.Literal["tonNode.persistentStateSizeNotFound"] = pydantic.Field(
        alias="@type", default="tonNode.persistentStateSizeNotFound"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_PersistentStateSize = typing.Annotated[
    tonNode_persistentStateSize | tonNode_persistentStateSizeNotFound,
    pydantic.Field(discriminator="tl_type"),
]
tonNode_PersistentStateSize_Model = pydantic.RootModel[tonNode_PersistentStateSize]


# ===== tonNode.Prepared =====
class tonNode_prepared(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcd\xbb\xc4\xea"
    tl_type: typing.Literal["tonNode.prepared"] = pydantic.Field(
        alias="@type", default="tonNode.prepared"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_notFound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa6\x3d\xc3\xe2"
    tl_type: typing.Literal["tonNode.notFound"] = pydantic.Field(
        alias="@type", default="tonNode.notFound"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_Prepared = typing.Annotated[
    tonNode_prepared | tonNode_notFound, pydantic.Field(discriminator="tl_type")
]
tonNode_Prepared_Model = pydantic.RootModel[tonNode_Prepared]


# ===== tonNode.PreparedProof =====
class tonNode_preparedProofEmpty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7a\xc1\x69\xc7"
    tl_type: typing.Literal["tonNode.preparedProofEmpty"] = pydantic.Field(
        alias="@type", default="tonNode.preparedProofEmpty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_preparedProof(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4b\x9a\x9f\x89"
    tl_type: typing.Literal["tonNode.preparedProof"] = pydantic.Field(
        alias="@type", default="tonNode.preparedProof"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_preparedProofLink(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8d\x32\xff\x3d"
    tl_type: typing.Literal["tonNode.preparedProofLink"] = pydantic.Field(
        alias="@type", default="tonNode.preparedProofLink"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_PreparedProof = typing.Annotated[
    tonNode_preparedProofEmpty | tonNode_preparedProof | tonNode_preparedProofLink,
    pydantic.Field(discriminator="tl_type"),
]
tonNode_PreparedProof_Model = pydantic.RootModel[tonNode_PreparedProof]


# ===== tonNode.PreparedState =====
class tonNode_preparedState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6d\xcb\x5b\x37"
    tl_type: typing.Literal["tonNode.preparedState"] = pydantic.Field(
        alias="@type", default="tonNode.preparedState"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tonNode_notFoundState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x51\x0a\x39\x32"
    tl_type: typing.Literal["tonNode.notFoundState"] = pydantic.Field(
        alias="@type", default="tonNode.notFoundState"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_PreparedState = typing.Annotated[
    tonNode_preparedState | tonNode_notFoundState, pydantic.Field(discriminator="tl_type")
]
tonNode_PreparedState_Model = pydantic.RootModel[tonNode_PreparedState]


# ===== tonNode.PrivateBlockOverlayId =====
class tonNode_privateBlockOverlayId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x62\xd8\xf4\xa6"
    tl_type: typing.Literal["tonNode.privateBlockOverlayId"] = pydantic.Field(
        alias="@type", default="tonNode.privateBlockOverlayId"
    )
    zero_state_file_hash: Int256 = b"\x00" * 32
    nodes: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_PrivateBlockOverlayId = tonNode_privateBlockOverlayId
tonNode_PrivateBlockOverlayId_Model = pydantic.RootModel[tonNode_PrivateBlockOverlayId]


# ===== tonNode.SessionId =====
class tonNode_sessionId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xba\x36\x92\x7a"
    tl_type: typing.Literal["tonNode.sessionId"] = pydantic.Field(
        alias="@type", default="tonNode.sessionId"
    )
    workchain: int = 0
    shard: int = 0
    cc_seqno: int = 0
    opts_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_SessionId = tonNode_sessionId
tonNode_SessionId_Model = pydantic.RootModel[tonNode_SessionId]


# ===== tonNode.ShardId =====
class tonNode_shardId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x77\x08\xfa\x28"
    tl_type: typing.Literal["tonNode.shardId"] = pydantic.Field(
        alias="@type", default="tonNode.shardId"
    )
    workchain: int = 0
    shard: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_ShardId = tonNode_shardId
tonNode_ShardId_Model = pydantic.RootModel[tonNode_ShardId]


# ===== tonNode.ShardPublicOverlayId =====
class tonNode_shardPublicOverlayId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x29\xd3\x9e\x4d"
    tl_type: typing.Literal["tonNode.shardPublicOverlayId"] = pydantic.Field(
        alias="@type", default="tonNode.shardPublicOverlayId"
    )
    workchain: int = 0
    shard: int = 0
    zero_state_file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_ShardPublicOverlayId = tonNode_shardPublicOverlayId
tonNode_ShardPublicOverlayId_Model = pydantic.RootModel[tonNode_ShardPublicOverlayId]


# ===== tonNode.Success =====
class tonNode_success(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\x24\x96\xc0"
    tl_type: typing.Literal["tonNode.success"] = pydantic.Field(
        alias="@type", default="tonNode.success"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_Success = tonNode_success
tonNode_Success_Model = pydantic.RootModel[tonNode_Success]


# ===== tonNode.ZeroStateIdExt =====
class tonNode_zeroStateIdExt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xae\x35\x72\x1d"
    tl_type: typing.Literal["tonNode.zeroStateIdExt"] = pydantic.Field(
        alias="@type", default="tonNode.zeroStateIdExt"
    )
    workchain: int = 0
    root_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_ZeroStateIdExt = tonNode_zeroStateIdExt
tonNode_ZeroStateIdExt_Model = pydantic.RootModel[tonNode_ZeroStateIdExt]


# ===== tonNode.blockBroadcaseCompressed.Data =====
class tonNode_blockBroadcastCompressed_data(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb6\x13\x91\xfe"
    tl_type: typing.Literal["tonNode.blockBroadcastCompressed.data"] = pydantic.Field(
        alias="@type", default="tonNode.blockBroadcastCompressed.data"
    )
    signatures: list["tonNode_BlockSignature"] = []
    proof_data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tonNode_blockBroadcaseCompressed_Data = tonNode_blockBroadcastCompressed_data
tonNode_blockBroadcaseCompressed_Data_Model = pydantic.RootModel[
    tonNode_blockBroadcaseCompressed_Data
]


# ===== validadorSession.CollationStats =====
class validatorSession_collationStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xee\x39\x2f\x69"
    tl_type: typing.Literal["validatorSession.collationStats"] = pydantic.Field(
        alias="@type", default="validatorSession.collationStats"
    )
    bytes: int = 0
    gas: int = 0
    lt_delta: int = 0
    cat_bytes: int = 0
    cat_gas: int = 0
    cat_lt_delta: int = 0
    limits_log: str = ""
    ext_msgs_total: int = 0
    ext_msgs_filtered: int = 0
    ext_msgs_accepted: int = 0
    ext_msgs_rejected: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validadorSession_CollationStats = validatorSession_collationStats
validadorSession_CollationStats_Model = pydantic.RootModel[validadorSession_CollationStats]


# ===== validator.Group =====
class validator_group(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa1\x7e\xd8\xf8"
    tl_type: typing.Literal["validator.group"] = pydantic.Field(
        alias="@type", default="validator.group"
    )
    workchain: int = 0
    shard: int = 0
    catchain_seqno: int = 0
    config_hash: Int256 = b"\x00" * 32
    members: list["engine_validator_GroupMember"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validator_groupEx(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfe\x4d\x92\x1c"
    tl_type: typing.Literal["validator.groupEx"] = pydantic.Field(
        alias="@type", default="validator.groupEx"
    )
    workchain: int = 0
    shard: int = 0
    vertical_seqno: int = 0
    catchain_seqno: int = 0
    config_hash: Int256 = b"\x00" * 32
    members: list["engine_validator_GroupMember"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validator_groupNew(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4d\xa1\x43\x98"
    tl_type: typing.Literal["validator.groupNew"] = pydantic.Field(
        alias="@type", default="validator.groupNew"
    )
    workchain: int = 0
    shard: int = 0
    vertical_seqno: int = 0
    last_key_block_seqno: int = 0
    catchain_seqno: int = 0
    config_hash: Int256 = b"\x00" * 32
    members: list["engine_validator_GroupMember"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validator_Group = typing.Annotated[
    validator_group | validator_groupEx | validator_groupNew,
    pydantic.Field(discriminator="tl_type"),
]
validator_Group_Model = pydantic.RootModel[validator_Group]
type validator_Telemetry = None  # unsupported


# ===== validator.config.Global =====
class validator_config_global(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6a\xff\x7d\x86"
    tl_type: typing.Literal["validator.config.global"] = pydantic.Field(
        alias="@type", default="validator.config.global"
    )
    zero_state: Option["tonNode_BlockIdExt"] = None
    init_block: Option["tonNode_BlockIdExt"] = None
    hardforks: list["tonNode_BlockIdExt"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validator_config_Global = validator_config_global
validator_config_Global_Model = pydantic.RootModel[validator_config_Global]


# ===== validator.config.Local =====
class validator_config_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x68\xff\x4b\x66"
    tl_type: typing.Literal["validator.config.local"] = pydantic.Field(
        alias="@type", default="validator.config.local"
    )
    id: Option["adnl_id_Short"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validator_config_random_local(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x62\x94\x83\x59"
    tl_type: typing.Literal["validator.config.random.local"] = pydantic.Field(
        alias="@type", default="validator.config.random.local"
    )
    addr_list: Option["adnl_AddressList"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validator_config_Local = typing.Annotated[
    validator_config_local | validator_config_random_local, pydantic.Field(discriminator="tl_type")
]
validator_config_Local_Model = pydantic.RootModel[validator_config_Local]


# ===== validatorSession.BlockUpdate =====
class validatorSession_blockUpdate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x37\xce\x83\x92"
    tl_type: typing.Literal["validatorSession.blockUpdate"] = pydantic.Field(
        alias="@type", default="validatorSession.blockUpdate"
    )
    ts: int = 0
    actions: list["validatorSession_round_Message"] = []
    state: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_BlockUpdate = validatorSession_blockUpdate
validatorSession_BlockUpdate_Model = pydantic.RootModel[validatorSession_BlockUpdate]


# ===== validatorSession.Candidate =====
class validatorSession_candidate(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x45\x78\x33\x7d"
    tl_type: typing.Literal["validatorSession.candidate"] = pydantic.Field(
        alias="@type", default="validatorSession.candidate"
    )
    src: Int256 = b"\x00" * 32
    round: int = 0
    root_hash: Int256 = b"\x00" * 32
    data: pydantic.Base64Bytes = b""
    collated_data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_Candidate = validatorSession_candidate
validatorSession_Candidate_Model = pydantic.RootModel[validatorSession_Candidate]


# ===== validatorSession.CandidateId =====
class validatorSession_candidateId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6c\xe5\xfe\x19"
    tl_type: typing.Literal["validatorSession.candidateId"] = pydantic.Field(
        alias="@type", default="validatorSession.candidateId"
    )
    src: Int256 = b"\x00" * 32
    root_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    collated_data_file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_CandidateId = validatorSession_candidateId
validatorSession_CandidateId_Model = pydantic.RootModel[validatorSession_CandidateId]


# ===== validatorSession.CatChainOptions =====
class validatorSession_catchainOptions(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe6\x49\xe2\x70"
    tl_type: typing.Literal["validatorSession.catchainOptions"] = pydantic.Field(
        alias="@type", default="validatorSession.catchainOptions"
    )
    idle_timeout: float = 0
    max_deps: int = 0
    max_block_size: int = 0
    block_hash_covers_data: bool = False
    max_block_height_ceoff: int = 0
    debug_disable_db: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_CatChainOptions = validatorSession_catchainOptions
validatorSession_CatChainOptions_Model = pydantic.RootModel[validatorSession_CatChainOptions]


# ===== validatorSession.Config =====
class validatorSession_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc3\xfd\x61\xb6"
    tl_type: typing.Literal["validatorSession.config"] = pydantic.Field(
        alias="@type", default="validatorSession.config"
    )
    catchain_idle_timeout: float = 0
    catchain_max_deps: int = 0
    round_candidates: int = 0
    next_candidate_delay: float = 0
    round_attempt_duration: int = 0
    max_round_attempts: int = 0
    max_block_size: int = 0
    max_collated_data_size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_configNew(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9c\xa9\xaf\xf7"
    tl_type: typing.Literal["validatorSession.configNew"] = pydantic.Field(
        alias="@type", default="validatorSession.configNew"
    )
    catchain_idle_timeout: float = 0
    catchain_max_deps: int = 0
    round_candidates: int = 0
    next_candidate_delay: float = 0
    round_attempt_duration: int = 0
    max_round_attempts: int = 0
    max_block_size: int = 0
    max_collated_data_size: int = 0
    new_catchain_ids: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_configVersioned(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x03\x97\x2a\x40"
    tl_type: typing.Literal["validatorSession.configVersioned"] = pydantic.Field(
        alias="@type", default="validatorSession.configVersioned"
    )
    catchain_idle_timeout: float = 0
    catchain_max_deps: int = 0
    round_candidates: int = 0
    next_candidate_delay: float = 0
    round_attempt_duration: int = 0
    max_round_attempts: int = 0
    max_block_size: int = 0
    max_collated_data_size: int = 0
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_configVersionedV2(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xaf\x11\x7b\xa9"
    tl_type: typing.Literal["validatorSession.configVersionedV2"] = pydantic.Field(
        alias="@type", default="validatorSession.configVersionedV2"
    )
    catchain_opts: Option["validatorSession_CatChainOptions"] = None
    round_candidates: int = 0
    next_candidate_delay: float = 0
    round_attempt_duration: int = 0
    max_round_attempts: int = 0
    max_block_size: int = 0
    max_collated_data_size: int = 0
    version: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_Config = typing.Annotated[
    validatorSession_config
    | validatorSession_configNew
    | validatorSession_configVersioned
    | validatorSession_configVersionedV2,
    pydantic.Field(discriminator="tl_type"),
]
validatorSession_Config_Model = pydantic.RootModel[validatorSession_Config]


# ===== validatorSession.EndValidatorGroupStats =====
class validatorSession_endValidatorGroupStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x96\xea\x32\x5f"
    tl_type: typing.Literal["validatorSession.endValidatorGroupStats"] = pydantic.Field(
        alias="@type", default="validatorSession.endValidatorGroupStats"
    )
    session_id: Int256 = b"\x00" * 32
    timestamp: float = 0
    nodes: list["validatorSession_endValidatorGroupStats_Node"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_EndValidatorGroupStats = validatorSession_endValidatorGroupStats
validatorSession_EndValidatorGroupStats_Model = pydantic.RootModel[
    validatorSession_EndValidatorGroupStats
]


# ===== validatorSession.Message =====
class validatorSession_message_startSession(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd1\x66\xa1\x96"
    tl_type: typing.Literal["validatorSession.message.startSession"] = pydantic.Field(
        alias="@type", default="validatorSession.message.startSession"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_finishSession(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe3\x22\x9b\xcb"
    tl_type: typing.Literal["validatorSession.message.finishSession"] = pydantic.Field(
        alias="@type", default="validatorSession.message.finishSession"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_Message = typing.Annotated[
    validatorSession_message_startSession | validatorSession_message_finishSession,
    pydantic.Field(discriminator="tl_type"),
]
validatorSession_Message_Model = pydantic.RootModel[validatorSession_Message]


# ===== validatorSession.NewValidatorGroupStats =====
class validatorSession_newValidatorGroupStats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa2\x32\x09\xc1"
    tl_type: typing.Literal["validatorSession.newValidatorGroupStats"] = pydantic.Field(
        alias="@type", default="validatorSession.newValidatorGroupStats"
    )
    session_id: Int256 = b"\x00" * 32
    workchain: int = 0
    shard: int = 0
    cc_seqno: int = 0
    last_key_block_seqno: int = 0
    timestamp: float = 0
    self_idx: int = 0
    nodes: list["validatorSession_newValidatorGroupStats_Node"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_NewValidatorGroupStats = validatorSession_newValidatorGroupStats
validatorSession_NewValidatorGroupStats_Model = pydantic.RootModel[
    validatorSession_NewValidatorGroupStats
]


# ===== validatorSession.Pong =====
class validatorSession_pong(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6d\x37\xc6\xdc"
    tl_type: typing.Literal["validatorSession.pong"] = pydantic.Field(
        alias="@type", default="validatorSession.pong"
    )
    hash: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_Pong = validatorSession_pong
validatorSession_Pong_Model = pydantic.RootModel[validatorSession_Pong]


# ===== validatorSession.Stats =====
class validatorSession_stats(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x30\xb7\x1f\x85"
    tl_type: typing.Literal["validatorSession.stats"] = pydantic.Field(
        alias="@type", default="validatorSession.stats"
    )
    success: bool = False
    id: Option["tonNode_BlockIdExt"] = None
    timestamp: float = 0
    self: Int256 = b"\x00" * 32
    session_id: Int256 = b"\x00" * 32
    cc_seqno: int = 0
    creator: Int256 = b"\x00" * 32
    total_validators: int = 0
    total_weight: int = 0
    signatures: int = 0
    signatures_weight: int = 0
    approve_signatures: int = 0
    approve_signatures_weight: int = 0
    first_round: int = 0
    rounds: list["validatorSession_StatsRound"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_Stats = validatorSession_stats
validatorSession_Stats_Model = pydantic.RootModel[validatorSession_Stats]


# ===== validatorSession.StatsProducer =====
class validatorSession_statsProducer(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x89\x93\x6c\x12"
    tl_type: typing.Literal["validatorSession.statsProducer"] = pydantic.Field(
        alias="@type", default="validatorSession.statsProducer"
    )
    id: Int256 = b"\x00" * 32
    candidate_id: Int256 = b"\x00" * 32
    block_status: int = 0
    root_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    comment: str = ""
    block_timestamp: float = 0
    is_accepted: bool = False
    is_ours: bool = False
    got_submit_at: float = 0
    collation_time: float = 0
    collated_at: float = 0
    collation_cached: bool = False
    collation_work_time: float = 0
    collation_cpu_work_time: float = 0
    collation_stats: Option["validadorSession_CollationStats"] = None
    validation_time: float = 0
    validated_at: float = 0
    validation_cached: bool = False
    validation_work_time: float = 0
    validation_cpu_work_time: float = 0
    gen_utime: float = 0
    approved_weight: int = 0
    approved_33pct_at: float = 0
    approved_66pct_at: float = 0
    approvers: str = ""
    signed_weight: int = 0
    signed_33pct_at: float = 0
    signed_66pct_at: float = 0
    signers: str = ""
    serialize_time: float = 0
    deserialize_time: float = 0
    serialized_size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_StatsProducer = validatorSession_statsProducer
validatorSession_StatsProducer_Model = pydantic.RootModel[validatorSession_StatsProducer]


# ===== validatorSession.StatsRound =====
class validatorSession_statsRound(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb2\xb7\xd9\x25"
    tl_type: typing.Literal["validatorSession.statsRound"] = pydantic.Field(
        alias="@type", default="validatorSession.statsRound"
    )
    timestamp: float = 0
    producers: list["validatorSession_StatsProducer"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_StatsRound = validatorSession_statsRound
validatorSession_StatsRound_Model = pydantic.RootModel[validatorSession_StatsRound]


# ===== validatorSession.endValidatorGroupStats.Node =====
class validatorSession_endValidatorGroupStats_node(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\x2b\x4c\xa2"
    tl_type: typing.Literal["validatorSession.endValidatorGroupStats.node"] = pydantic.Field(
        alias="@type", default="validatorSession.endValidatorGroupStats.node"
    )
    id: Int256 = b"\x00" * 32
    catchain_blocks: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_endValidatorGroupStats_Node = validatorSession_endValidatorGroupStats_node
validatorSession_endValidatorGroupStats_Node_Model = pydantic.RootModel[
    validatorSession_endValidatorGroupStats_Node
]


# ===== validatorSession.newValidatorGroupStats.Node =====
class validatorSession_newValidatorGroupStats_node(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf8\x33\xdc\x2d"
    tl_type: typing.Literal["validatorSession.newValidatorGroupStats.node"] = pydantic.Field(
        alias="@type", default="validatorSession.newValidatorGroupStats.node"
    )
    id: Int256 = b"\x00" * 32
    weight: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_newValidatorGroupStats_Node = validatorSession_newValidatorGroupStats_node
validatorSession_newValidatorGroupStats_Node_Model = pydantic.RootModel[
    validatorSession_newValidatorGroupStats_Node
]


# ===== validatorSession.round.Id =====
class validatorSession_round_id(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa5\xcf\x25\x00"
    tl_type: typing.Literal["validatorSession.round.id"] = pydantic.Field(
        alias="@type", default="validatorSession.round.id"
    )
    session: Int256 = b"\x00" * 32
    height: int = 0
    prev_block: Int256 = b"\x00" * 32
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_round_Id = validatorSession_round_id
validatorSession_round_Id_Model = pydantic.RootModel[validatorSession_round_Id]


# ===== validatorSession.round.Message =====
class validatorSession_message_submittedBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb6\x24\x76\x12"
    tl_type: typing.Literal["validatorSession.message.submittedBlock"] = pydantic.Field(
        alias="@type", default="validatorSession.message.submittedBlock"
    )
    round: int = 0
    root_hash: Int256 = b"\x00" * 32
    file_hash: Int256 = b"\x00" * 32
    collated_data_file_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_approvedBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x81\xb5\xa5\x04"
    tl_type: typing.Literal["validatorSession.message.approvedBlock"] = pydantic.Field(
        alias="@type", default="validatorSession.message.approvedBlock"
    )
    round: int = 0
    candidate: Int256 = b"\x00" * 32
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_rejectedBlock(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6b\x4e\x88\x95"
    tl_type: typing.Literal["validatorSession.message.rejectedBlock"] = pydantic.Field(
        alias="@type", default="validatorSession.message.rejectedBlock"
    )
    round: int = 0
    candidate: Int256 = b"\x00" * 32
    reason: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_commit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf5\x9e\x12\xac"
    tl_type: typing.Literal["validatorSession.message.commit"] = pydantic.Field(
        alias="@type", default="validatorSession.message.commit"
    )
    round: int = 0
    candidate: Int256 = b"\x00" * 32
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_vote(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc7\x51\x32\x9a"
    tl_type: typing.Literal["validatorSession.message.vote"] = pydantic.Field(
        alias="@type", default="validatorSession.message.vote"
    )
    round: int = 0
    attempt: int = 0
    candidate: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_voteFor(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2f\xfe\xf0\x61"
    tl_type: typing.Literal["validatorSession.message.voteFor"] = pydantic.Field(
        alias="@type", default="validatorSession.message.voteFor"
    )
    round: int = 0
    attempt: int = 0
    candidate: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_precommit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x52\xb5\x54\xa8"
    tl_type: typing.Literal["validatorSession.message.precommit"] = pydantic.Field(
        alias="@type", default="validatorSession.message.precommit"
    )
    round: int = 0
    attempt: int = 0
    candidate: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class validatorSession_message_empty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa9\x1f\x20\x4a"
    tl_type: typing.Literal["validatorSession.message.empty"] = pydantic.Field(
        alias="@type", default="validatorSession.message.empty"
    )
    round: int = 0
    attempt: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_round_Message = typing.Annotated[
    validatorSession_message_submittedBlock
    | validatorSession_message_approvedBlock
    | validatorSession_message_rejectedBlock
    | validatorSession_message_commit
    | validatorSession_message_vote
    | validatorSession_message_voteFor
    | validatorSession_message_precommit
    | validatorSession_message_empty,
    pydantic.Field(discriminator="tl_type"),
]
validatorSession_round_Message_Model = pydantic.RootModel[validatorSession_round_Message]


# ===== validatorSession.tempBlock.Id =====
class validatorSession_candidate_id(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x39\x41\xd7\xbc"
    tl_type: typing.Literal["validatorSession.candidate.id"] = pydantic.Field(
        alias="@type", default="validatorSession.candidate.id"
    )
    round: Int256 = b"\x00" * 32
    block_hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type validatorSession_tempBlock_Id = validatorSession_candidate_id
validatorSession_tempBlock_Id_Model = pydantic.RootModel[validatorSession_tempBlock_Id]
