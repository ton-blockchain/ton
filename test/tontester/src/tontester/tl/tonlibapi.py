import typing

import pydantic

type Int128 = typing.Annotated[pydantic.Base64Bytes, pydantic.Field(min_length=16, max_length=16)]
type Int256 = typing.Annotated[pydantic.Base64Bytes, pydantic.Field(min_length=32, max_length=32)]
type Option[T] = T | None


# ===== AccountAddress =====
class accountAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xab\xbd\x09\x2d"
    tl_type: typing.Literal["accountAddress"] = pydantic.Field(
        alias="@type", default="accountAddress"
    )
    account_address: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type AccountAddress = accountAddress
AccountAddress_Model = pydantic.RootModel[AccountAddress]


# ===== AccountList =====
class accountList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x55\xb2\x3e\x78"
    tl_type: typing.Literal["accountList"] = pydantic.Field(alias="@type", default="accountList")
    accounts: list["FullAccountState"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type AccountList = accountList
AccountList_Model = pydantic.RootModel[AccountList]


# ===== AccountRevisionList =====
class accountRevisionList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xca\x64\x6c\x1f"
    tl_type: typing.Literal["accountRevisionList"] = pydantic.Field(
        alias="@type", default="accountRevisionList"
    )
    revisions: list["FullAccountState"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type AccountRevisionList = accountRevisionList
AccountRevisionList_Model = pydantic.RootModel[AccountRevisionList]


# ===== AccountState =====
class raw_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3a\x96\x4b\xe0"
    tl_type: typing.Literal["raw.accountState"] = pydantic.Field(
        alias="@type", default="raw.accountState"
    )
    code: pydantic.Base64Bytes = b""
    data: pydantic.Base64Bytes = b""
    frozen_hash: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_v3_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4a\xa8\x7a\x9f"
    tl_type: typing.Literal["wallet.v3.accountState"] = pydantic.Field(
        alias="@type", default="wallet.v3.accountState"
    )
    wallet_id: int = 0
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_v4_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x76\x97\x2e\xd4"
    tl_type: typing.Literal["wallet.v4.accountState"] = pydantic.Field(
        alias="@type", default="wallet.v4.accountState"
    )
    wallet_id: int = 0
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_highload_v1_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xdc\xe4\x57\x60"
    tl_type: typing.Literal["wallet.highload.v1.accountState"] = pydantic.Field(
        alias="@type", default="wallet.highload.v1.accountState"
    )
    wallet_id: int = 0
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_highload_v2_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\x5d\x7d\x94"
    tl_type: typing.Literal["wallet.highload.v2.accountState"] = pydantic.Field(
        alias="@type", default="wallet.highload.v2.accountState"
    )
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6a\xd8\xfa\x66"
    tl_type: typing.Literal["dns.accountState"] = pydantic.Field(
        alias="@type", default="dns.accountState"
    )
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rwallet_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd8\x83\xeb\xd3"
    tl_type: typing.Literal["rwallet.accountState"] = pydantic.Field(
        alias="@type", default="rwallet.accountState"
    )
    wallet_id: int = 0
    seqno: int = 0
    unlocked_balance: int = 0
    config: Option["rwallet_Config"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pchan_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x78\x6f\x22\x60"
    tl_type: typing.Literal["pchan.accountState"] = pydantic.Field(
        alias="@type", default="pchan.accountState"
    )
    config: Option["pchan_Config"] = None
    state: Option["pchan_State"] = None
    description: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class uninited_accountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x08\x97\xbd\x5a"
    tl_type: typing.Literal["uninited.accountState"] = pydantic.Field(
        alias="@type", default="uninited.accountState"
    )
    frozen_hash: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type AccountState = typing.Annotated[
    raw_accountState
    | wallet_v3_accountState
    | wallet_v4_accountState
    | wallet_highload_v1_accountState
    | wallet_highload_v2_accountState
    | dns_accountState
    | rwallet_accountState
    | pchan_accountState
    | uninited_accountState,
    pydantic.Field(discriminator="tl_type"),
]
AccountState_Model = pydantic.RootModel[AccountState]


# ===== Action =====
class actionNoop(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9b\xac\xb3\x43"
    tl_type: typing.Literal["actionNoop"] = pydantic.Field(alias="@type", default="actionNoop")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class actionMsg(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x50\x77\xb6\x0e"
    tl_type: typing.Literal["actionMsg"] = pydantic.Field(alias="@type", default="actionMsg")
    messages: list["msg_Message"] = []
    allow_send_to_uninited: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class actionDns(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x21\x30\x27\x47"
    tl_type: typing.Literal["actionDns"] = pydantic.Field(alias="@type", default="actionDns")
    actions: list["dns_Action"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class actionPchan(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe1\xc5\x2d\xa7"
    tl_type: typing.Literal["actionPchan"] = pydantic.Field(alias="@type", default="actionPchan")
    action: Option["pchan_Action"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class actionRwallet(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc5\x37\x02\xf9"
    tl_type: typing.Literal["actionRwallet"] = pydantic.Field(
        alias="@type", default="actionRwallet"
    )
    action: Option["rwallet_Action"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Action = typing.Annotated[
    actionNoop | actionMsg | actionDns | actionPchan | actionRwallet,
    pydantic.Field(discriminator="tl_type"),
]
Action_Model = pydantic.RootModel[Action]


# ===== AdnlAddress =====
class adnlAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0c\x95\x31\x04"
    tl_type: typing.Literal["adnlAddress"] = pydantic.Field(alias="@type", default="adnlAddress")
    adnl_address: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type AdnlAddress = adnlAddress
AdnlAddress_Model = pydantic.RootModel[AdnlAddress]


# ===== Bip39Hints =====
class bip39Hints(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x00\x9c\x55\x3c"
    tl_type: typing.Literal["bip39Hints"] = pydantic.Field(alias="@type", default="bip39Hints")
    words: list[str] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Bip39Hints = bip39Hints
Bip39Hints_Model = pydantic.RootModel[Bip39Hints]


# ===== Config =====
class config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x38\x02\x4e\xa4"
    tl_type: typing.Literal["config"] = pydantic.Field(alias="@type", default="config")
    config: str = ""
    blockchain_name: str = ""
    use_callbacks_for_network: bool = False
    ignore_cache: bool = False
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Config = config
Config_Model = pydantic.RootModel[Config]


# ===== ConfigInfo =====
class configInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xff\x55\x00\x29"
    tl_type: typing.Literal["configInfo"] = pydantic.Field(alias="@type", default="configInfo")
    config: Option["tvm_Cell"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ConfigInfo = configInfo
ConfigInfo_Model = pydantic.RootModel[ConfigInfo]


# ===== Data =====
class data(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x71\xa9\x47\xe7"
    tl_type: typing.Literal["data"] = pydantic.Field(alias="@type", default="data")
    bytes: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Data = data
Data_Model = pydantic.RootModel[Data]


# ===== Error =====
class error(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1a\x8f\xdd\x9b"
    tl_type: typing.Literal["error"] = pydantic.Field(alias="@type", default="error")
    code: int = 0
    message: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Error = error
Error_Model = pydantic.RootModel[Error]


# ===== ExportedEncryptedKey =====
class exportedEncryptedKey(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x54\xfe\xa9\x78"
    tl_type: typing.Literal["exportedEncryptedKey"] = pydantic.Field(
        alias="@type", default="exportedEncryptedKey"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ExportedEncryptedKey = exportedEncryptedKey
ExportedEncryptedKey_Model = pydantic.RootModel[ExportedEncryptedKey]


# ===== ExportedKey =====
class exportedKey(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd7\x39\x9e\xa9"
    tl_type: typing.Literal["exportedKey"] = pydantic.Field(alias="@type", default="exportedKey")
    word_list: list[str] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ExportedKey = exportedKey
ExportedKey_Model = pydantic.RootModel[ExportedKey]


# ===== ExportedPemKey =====
class exportedPemKey(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbd\x00\xf7\x54"
    tl_type: typing.Literal["exportedPemKey"] = pydantic.Field(
        alias="@type", default="exportedPemKey"
    )
    pem: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ExportedPemKey = exportedPemKey
ExportedPemKey_Model = pydantic.RootModel[ExportedPemKey]


# ===== ExportedUnencryptedKey =====
class exportedUnencryptedKey(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe8\x9a\x83\x2b"
    tl_type: typing.Literal["exportedUnencryptedKey"] = pydantic.Field(
        alias="@type", default="exportedUnencryptedKey"
    )
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ExportedUnencryptedKey = exportedUnencryptedKey
ExportedUnencryptedKey_Model = pydantic.RootModel[ExportedUnencryptedKey]


# ===== ExtraCurrency =====
class extraCurrency(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd1\x25\xaf\x14"
    tl_type: typing.Literal["extraCurrency"] = pydantic.Field(
        alias="@type", default="extraCurrency"
    )
    id: int = 0
    amount: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ExtraCurrency = extraCurrency
ExtraCurrency_Model = pydantic.RootModel[ExtraCurrency]


# ===== Fees =====
class fees(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbc\xe6\xe9\x63"
    tl_type: typing.Literal["fees"] = pydantic.Field(alias="@type", default="fees")
    in_fwd_fee: int = 0
    storage_fee: int = 0
    gas_fee: int = 0
    fwd_fee: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Fees = fees
Fees_Model = pydantic.RootModel[Fees]


# ===== FullAccountState =====
class fullAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x69\x62\xa7\xd7"
    tl_type: typing.Literal["fullAccountState"] = pydantic.Field(
        alias="@type", default="fullAccountState"
    )
    address: Option["AccountAddress"] = None
    balance: int = 0
    extra_currencies: list["ExtraCurrency"] = []
    last_transaction_id: Option["internal_TransactionId"] = None
    block_id: Option["ton_BlockIdExt"] = None
    sync_utime: int = 0
    account_state: Option["AccountState"] = None
    revision: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type FullAccountState = fullAccountState
FullAccountState_Model = pydantic.RootModel[FullAccountState]


# ===== InitialAccountState =====
class raw_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x47\x5c\xdb\xeb"
    tl_type: typing.Literal["raw.initialAccountState"] = pydantic.Field(
        alias="@type", default="raw.initialAccountState"
    )
    code: pydantic.Base64Bytes = b""
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_v3_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x40\x55\xf6\xf8"
    tl_type: typing.Literal["wallet.v3.initialAccountState"] = pydantic.Field(
        alias="@type", default="wallet.v3.initialAccountState"
    )
    public_key: str = ""
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_v4_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc5\xc2\xe9\xd0"
    tl_type: typing.Literal["wallet.v4.initialAccountState"] = pydantic.Field(
        alias="@type", default="wallet.v4.initialAccountState"
    )
    public_key: str = ""
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_highload_v1_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x46\x9e\x74\xec"
    tl_type: typing.Literal["wallet.highload.v1.initialAccountState"] = pydantic.Field(
        alias="@type", default="wallet.highload.v1.initialAccountState"
    )
    public_key: str = ""
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class wallet_highload_v2_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x29\x79\x34\x75"
    tl_type: typing.Literal["wallet.highload.v2.initialAccountState"] = pydantic.Field(
        alias="@type", default="wallet.highload.v2.initialAccountState"
    )
    public_key: str = ""
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class rwallet_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x14\x0c\xb9\x45"
    tl_type: typing.Literal["rwallet.initialAccountState"] = pydantic.Field(
        alias="@type", default="rwallet.initialAccountState"
    )
    init_public_key: str = ""
    public_key: str = ""
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbf\xa4\xcb\x6d"
    tl_type: typing.Literal["dns.initialAccountState"] = pydantic.Field(
        alias="@type", default="dns.initialAccountState"
    )
    public_key: str = ""
    wallet_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pchan_initialAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x44\x1d\x3e\xb2"
    tl_type: typing.Literal["pchan.initialAccountState"] = pydantic.Field(
        alias="@type", default="pchan.initialAccountState"
    )
    config: Option["pchan_Config"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type InitialAccountState = typing.Annotated[
    raw_initialAccountState
    | wallet_v3_initialAccountState
    | wallet_v4_initialAccountState
    | wallet_highload_v1_initialAccountState
    | wallet_highload_v2_initialAccountState
    | rwallet_initialAccountState
    | dns_initialAccountState
    | pchan_initialAccountState,
    pydantic.Field(discriminator="tl_type"),
]
InitialAccountState_Model = pydantic.RootModel[InitialAccountState]


# ===== InputKey =====
class inputKeyRegular(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9e\x46\xe5\xde"
    tl_type: typing.Literal["inputKeyRegular"] = pydantic.Field(
        alias="@type", default="inputKeyRegular"
    )
    key: Option["Key"] = None
    local_password: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class inputKeyFake(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbe\x39\xfb\xbf"
    tl_type: typing.Literal["inputKeyFake"] = pydantic.Field(alias="@type", default="inputKeyFake")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type InputKey = typing.Annotated[
    inputKeyRegular | inputKeyFake, pydantic.Field(discriminator="tl_type")
]
InputKey_Model = pydantic.RootModel[InputKey]


# ===== Key =====
class key(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd5\x93\x14\x8a"
    tl_type: typing.Literal["key"] = pydantic.Field(alias="@type", default="key")
    public_key: str = ""
    secret: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Key = key
Key_Model = pydantic.RootModel[Key]


# ===== KeyStoreType =====
class keyStoreTypeDirectory(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x2a\x12\x69\xe9"
    tl_type: typing.Literal["keyStoreTypeDirectory"] = pydantic.Field(
        alias="@type", default="keyStoreTypeDirectory"
    )
    directory: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class keyStoreTypeInMemory(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc7\x09\x6c\x82"
    tl_type: typing.Literal["keyStoreTypeInMemory"] = pydantic.Field(
        alias="@type", default="keyStoreTypeInMemory"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type KeyStoreType = typing.Annotated[
    keyStoreTypeDirectory | keyStoreTypeInMemory, pydantic.Field(discriminator="tl_type")
]
KeyStoreType_Model = pydantic.RootModel[KeyStoreType]


# ===== LogStream =====
class logStreamDefault(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbc\x96\xe2\x52"
    tl_type: typing.Literal["logStreamDefault"] = pydantic.Field(
        alias="@type", default="logStreamDefault"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class logStreamFile(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x56\x2a\xf0\x8f"
    tl_type: typing.Literal["logStreamFile"] = pydantic.Field(
        alias="@type", default="logStreamFile"
    )
    path: str = ""
    max_file_size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class logStreamEmpty(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xcc\xf1\x33\xe2"
    tl_type: typing.Literal["logStreamEmpty"] = pydantic.Field(
        alias="@type", default="logStreamEmpty"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type LogStream = typing.Annotated[
    logStreamDefault | logStreamFile | logStreamEmpty, pydantic.Field(discriminator="tl_type")
]
LogStream_Model = pydantic.RootModel[LogStream]


# ===== LogTags =====
class logTags(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xd7\xb3\x56\xa0"
    tl_type: typing.Literal["logTags"] = pydantic.Field(alias="@type", default="logTags")
    tags: list[str] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type LogTags = logTags
LogTags_Model = pydantic.RootModel[LogTags]


# ===== LogVerbosityLevel =====
class logVerbosityLevel(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xea\x43\x64\x67"
    tl_type: typing.Literal["logVerbosityLevel"] = pydantic.Field(
        alias="@type", default="logVerbosityLevel"
    )
    verbosity_level: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type LogVerbosityLevel = logVerbosityLevel
LogVerbosityLevel_Model = pydantic.RootModel[LogVerbosityLevel]


# ===== Ok =====
class ok(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x69\xbe\xed\xd4"
    tl_type: typing.Literal["ok"] = pydantic.Field(alias="@type", default="ok")
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Ok = ok
Ok_Model = pydantic.RootModel[Ok]


# ===== Options =====
class options(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf9\x29\x4c\x8d"
    tl_type: typing.Literal["options"] = pydantic.Field(alias="@type", default="options")
    config: Option["Config"] = None
    keystore_type: Option["KeyStoreType"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Options = options
Options_Model = pydantic.RootModel[Options]


# ===== SyncState =====
class syncStateDone(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x09\x39\xf3\x53"
    tl_type: typing.Literal["syncStateDone"] = pydantic.Field(
        alias="@type", default="syncStateDone"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class syncStateInProgress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc7\xc4\x6b\x06"
    tl_type: typing.Literal["syncStateInProgress"] = pydantic.Field(
        alias="@type", default="syncStateInProgress"
    )
    from_seqno: int = 0
    to_seqno: int = 0
    current_seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type SyncState = typing.Annotated[
    syncStateDone | syncStateInProgress, pydantic.Field(discriminator="tl_type")
]
SyncState_Model = pydantic.RootModel[SyncState]


# ===== UnpackedAccountAddress =====
class unpackedAccountAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x36\x14\xd4\x70"
    tl_type: typing.Literal["unpackedAccountAddress"] = pydantic.Field(
        alias="@type", default="unpackedAccountAddress"
    )
    workchain_id: int = 0
    bounceable: bool = False
    testnet: bool = False
    addr: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type UnpackedAccountAddress = unpackedAccountAddress
UnpackedAccountAddress_Model = pydantic.RootModel[UnpackedAccountAddress]


# ===== Update =====
class updateSendLiteServerQuery(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xdc\x95\x4e\xa3"
    tl_type: typing.Literal["updateSendLiteServerQuery"] = pydantic.Field(
        alias="@type", default="updateSendLiteServerQuery"
    )
    id: int = 0
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class updateSyncState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xde\x23\xc8\x47"
    tl_type: typing.Literal["updateSyncState"] = pydantic.Field(
        alias="@type", default="updateSyncState"
    )
    sync_state: Option["SyncState"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type Update = typing.Annotated[
    updateSendLiteServerQuery | updateSyncState, pydantic.Field(discriminator="tl_type")
]
Update_Model = pydantic.RootModel[Update]


# ===== blocks.AccountTransactionId =====
class blocks_accountTransactionId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x84\x22\xf7\xcb"
    tl_type: typing.Literal["blocks.accountTransactionId"] = pydantic.Field(
        alias="@type", default="blocks.accountTransactionId"
    )
    account: pydantic.Base64Bytes = b""
    lt: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_AccountTransactionId = blocks_accountTransactionId
blocks_AccountTransactionId_Model = pydantic.RootModel[blocks_AccountTransactionId]


# ===== blocks.BlockLinkBack =====
class blocks_blockLinkBack(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x35\x80\x8c\x41"
    tl_type: typing.Literal["blocks.blockLinkBack"] = pydantic.Field(
        alias="@type", default="blocks.blockLinkBack"
    )
    to_key_block: bool = False
    from_: Option["ton_BlockIdExt"] = None
    to: Option["ton_BlockIdExt"] = None
    dest_proof: pydantic.Base64Bytes = b""
    proof: pydantic.Base64Bytes = b""
    state_proof: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_BlockLinkBack = blocks_blockLinkBack
blocks_BlockLinkBack_Model = pydantic.RootModel[blocks_BlockLinkBack]


# ===== blocks.BlockSignatures =====
class blocks_blockSignatures(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9b\xdb\x01\xe8"
    tl_type: typing.Literal["blocks.blockSignatures"] = pydantic.Field(
        alias="@type", default="blocks.blockSignatures"
    )
    id: Option["ton_BlockIdExt"] = None
    signatures: list["blocks_Signature"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_BlockSignatures = blocks_blockSignatures
blocks_BlockSignatures_Model = pydantic.RootModel[blocks_BlockSignatures]
type blocks_Header = None  # unsupported


# ===== blocks.MasterchainInfo =====
class blocks_masterchainInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4b\x43\xca\x31"
    tl_type: typing.Literal["blocks.masterchainInfo"] = pydantic.Field(
        alias="@type", default="blocks.masterchainInfo"
    )
    last: Option["ton_BlockIdExt"] = None
    state_root_hash: pydantic.Base64Bytes = b""
    init: Option["ton_BlockIdExt"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_MasterchainInfo = blocks_masterchainInfo
blocks_MasterchainInfo_Model = pydantic.RootModel[blocks_MasterchainInfo]


# ===== blocks.OutMsgQueueSize =====
class blocks_outMsgQueueSize(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4f\x8b\x60\x62"
    tl_type: typing.Literal["blocks.outMsgQueueSize"] = pydantic.Field(
        alias="@type", default="blocks.outMsgQueueSize"
    )
    id: Option["ton_BlockIdExt"] = None
    size: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_OutMsgQueueSize = blocks_outMsgQueueSize
blocks_OutMsgQueueSize_Model = pydantic.RootModel[blocks_OutMsgQueueSize]


# ===== blocks.OutMsgQueueSizes =====
class blocks_outMsgQueueSizes(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x42\x8d\x03\xdd"
    tl_type: typing.Literal["blocks.outMsgQueueSizes"] = pydantic.Field(
        alias="@type", default="blocks.outMsgQueueSizes"
    )
    shards: list["blocks_OutMsgQueueSize"] = []
    ext_msg_queue_size_limit: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_OutMsgQueueSizes = blocks_outMsgQueueSizes
blocks_OutMsgQueueSizes_Model = pydantic.RootModel[blocks_OutMsgQueueSizes]


# ===== blocks.ShardBlockLink =====
class blocks_shardBlockLink(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x69\x15\xe0\xa6"
    tl_type: typing.Literal["blocks.shardBlockLink"] = pydantic.Field(
        alias="@type", default="blocks.shardBlockLink"
    )
    id: Option["ton_BlockIdExt"] = None
    proof: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_ShardBlockLink = blocks_shardBlockLink
blocks_ShardBlockLink_Model = pydantic.RootModel[blocks_ShardBlockLink]


# ===== blocks.ShardBlockProof =====
class blocks_shardBlockProof(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x53\x5f\xd6\xfb"
    tl_type: typing.Literal["blocks.shardBlockProof"] = pydantic.Field(
        alias="@type", default="blocks.shardBlockProof"
    )
    from_: Option["ton_BlockIdExt"] = None
    mc_id: Option["ton_BlockIdExt"] = None
    links: list["blocks_ShardBlockLink"] = []
    mc_proof: list["blocks_BlockLinkBack"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_ShardBlockProof = blocks_shardBlockProof
blocks_ShardBlockProof_Model = pydantic.RootModel[blocks_ShardBlockProof]


# ===== blocks.Shards =====
class blocks_shards(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x4a\xa9\x59\x7b"
    tl_type: typing.Literal["blocks.shards"] = pydantic.Field(
        alias="@type", default="blocks.shards"
    )
    shards: list["ton_BlockIdExt"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_Shards = blocks_shards
blocks_Shards_Model = pydantic.RootModel[blocks_Shards]


# ===== blocks.Signature =====
class blocks_signature(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc1\x88\x12\xb7"
    tl_type: typing.Literal["blocks.signature"] = pydantic.Field(
        alias="@type", default="blocks.signature"
    )
    node_id_short: Int256 = b"\x00" * 32
    signature: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_Signature = blocks_signature
blocks_Signature_Model = pydantic.RootModel[blocks_Signature]


# ===== blocks.Transactions =====
class blocks_transactions(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x94\xf0\x6b\x8d"
    tl_type: typing.Literal["blocks.transactions"] = pydantic.Field(
        alias="@type", default="blocks.transactions"
    )
    id: Option["ton_BlockIdExt"] = None
    req_count: int = 0
    incomplete: bool = False
    transactions: list["liteServer_TransactionId"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_Transactions = blocks_transactions
blocks_Transactions_Model = pydantic.RootModel[blocks_Transactions]


# ===== blocks.TransactionsExt =====
class blocks_transactionsExt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1d\x4e\xc9\x4f"
    tl_type: typing.Literal["blocks.transactionsExt"] = pydantic.Field(
        alias="@type", default="blocks.transactionsExt"
    )
    id: Option["ton_BlockIdExt"] = None
    req_count: int = 0
    incomplete: bool = False
    transactions: list["raw_Transaction"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type blocks_TransactionsExt = blocks_transactionsExt
blocks_TransactionsExt_Model = pydantic.RootModel[blocks_TransactionsExt]


# ===== dns.Action =====
class dns_actionDeleteAll(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9e\x90\x9e\x3f"
    tl_type: typing.Literal["dns.actionDeleteAll"] = pydantic.Field(
        alias="@type", default="dns.actionDeleteAll"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_actionDelete(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x51\x7f\x07\x44"
    tl_type: typing.Literal["dns.actionDelete"] = pydantic.Field(
        alias="@type", default="dns.actionDelete"
    )
    name: str = ""
    category: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_actionSet(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc3\xb1\x0b\xae"
    tl_type: typing.Literal["dns.actionSet"] = pydantic.Field(
        alias="@type", default="dns.actionSet"
    )
    entry: Option["dns_Entry"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dns_Action = typing.Annotated[
    dns_actionDeleteAll | dns_actionDelete | dns_actionSet, pydantic.Field(discriminator="tl_type")
]
dns_Action_Model = pydantic.RootModel[dns_Action]


# ===== dns.Entry =====
class dns_entry(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa6\x47\x1b\x1e"
    tl_type: typing.Literal["dns.entry"] = pydantic.Field(alias="@type", default="dns.entry")
    name: str = ""
    category: Int256 = b"\x00" * 32
    entry: Option["dns_EntryData"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dns_Entry = dns_entry
dns_Entry_Model = pydantic.RootModel[dns_Entry]


# ===== dns.EntryData =====
class dns_entryDataUnknown(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x80\xd3\x5a\xb3"
    tl_type: typing.Literal["dns.entryDataUnknown"] = pydantic.Field(
        alias="@type", default="dns.entryDataUnknown"
    )
    bytes: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_entryDataText(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x12\xa1\xc3\xd0"
    tl_type: typing.Literal["dns.entryDataText"] = pydantic.Field(
        alias="@type", default="dns.entryDataText"
    )
    text: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_entryDataNextResolver(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xc8\x3d\xb1\x13"
    tl_type: typing.Literal["dns.entryDataNextResolver"] = pydantic.Field(
        alias="@type", default="dns.entryDataNextResolver"
    )
    resolver: Option["AccountAddress"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_entryDataSmcAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x42\x7a\x19\x97"
    tl_type: typing.Literal["dns.entryDataSmcAddress"] = pydantic.Field(
        alias="@type", default="dns.entryDataSmcAddress"
    )
    smc_address: Option["AccountAddress"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_entryDataAdnlAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x10\xba\x98\xbd"
    tl_type: typing.Literal["dns.entryDataAdnlAddress"] = pydantic.Field(
        alias="@type", default="dns.entryDataAdnlAddress"
    )
    adnl_address: Option["AdnlAddress"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class dns_entryDataStorageAddress(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x1c\x54\xa0\x97"
    tl_type: typing.Literal["dns.entryDataStorageAddress"] = pydantic.Field(
        alias="@type", default="dns.entryDataStorageAddress"
    )
    bag_id: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dns_EntryData = typing.Annotated[
    dns_entryDataUnknown
    | dns_entryDataText
    | dns_entryDataNextResolver
    | dns_entryDataSmcAddress
    | dns_entryDataAdnlAddress
    | dns_entryDataStorageAddress,
    pydantic.Field(discriminator="tl_type"),
]
dns_EntryData_Model = pydantic.RootModel[dns_EntryData]


# ===== dns.Resolved =====
class dns_resolved(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x96\x05\x97\x7c"
    tl_type: typing.Literal["dns.resolved"] = pydantic.Field(alias="@type", default="dns.resolved")
    entries: list["dns_Entry"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type dns_Resolved = dns_resolved
dns_Resolved_Model = pydantic.RootModel[dns_Resolved]


# ===== internal.BlockId =====
class ton_blockId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa2\x7f\x58\xb9"
    tl_type: typing.Literal["ton.blockId"] = pydantic.Field(alias="@type", default="ton.blockId")
    workchain: int = 0
    shard: int = 0
    seqno: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type internal_BlockId = ton_blockId
internal_BlockId_Model = pydantic.RootModel[internal_BlockId]


# ===== internal.TransactionId =====
class internal_transactionId(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x22\x03\x05\xc5"
    tl_type: typing.Literal["internal.transactionId"] = pydantic.Field(
        alias="@type", default="internal.transactionId"
    )
    lt: int = 0
    hash: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type internal_TransactionId = internal_transactionId
internal_TransactionId_Model = pydantic.RootModel[internal_TransactionId]


# ===== liteServer.Info =====
class liteServer_info(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x73\xfe\x7b\xb5"
    tl_type: typing.Literal["liteServer.info"] = pydantic.Field(
        alias="@type", default="liteServer.info"
    )
    now: int = 0
    version: int = 0
    capabilities: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type liteServer_Info = liteServer_info
liteServer_Info_Model = pydantic.RootModel[liteServer_Info]
type liteServer_TransactionId = None  # unsupported


# ===== msg.Data =====
class msg_dataRaw(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x76\x5d\x06\x8d"
    tl_type: typing.Literal["msg.dataRaw"] = pydantic.Field(alias="@type", default="msg.dataRaw")
    body: pydantic.Base64Bytes = b""
    init_state: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class msg_dataText(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x90\x32\xa4\xeb"
    tl_type: typing.Literal["msg.dataText"] = pydantic.Field(alias="@type", default="msg.dataText")
    text: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class msg_dataDecryptedText(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb9\x60\x29\xb3"
    tl_type: typing.Literal["msg.dataDecryptedText"] = pydantic.Field(
        alias="@type", default="msg.dataDecryptedText"
    )
    text: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class msg_dataEncryptedText(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xda\x0b\x52\xee"
    tl_type: typing.Literal["msg.dataEncryptedText"] = pydantic.Field(
        alias="@type", default="msg.dataEncryptedText"
    )
    text: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type msg_Data = typing.Annotated[
    msg_dataRaw | msg_dataText | msg_dataDecryptedText | msg_dataEncryptedText,
    pydantic.Field(discriminator="tl_type"),
]
msg_Data_Model = pydantic.RootModel[msg_Data]


# ===== msg.DataDecrypted =====
class msg_dataDecrypted(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe9\x60\xa9\x0b"
    tl_type: typing.Literal["msg.dataDecrypted"] = pydantic.Field(
        alias="@type", default="msg.dataDecrypted"
    )
    proof: pydantic.Base64Bytes = b""
    data: Option["msg_Data"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type msg_DataDecrypted = msg_dataDecrypted
msg_DataDecrypted_Model = pydantic.RootModel[msg_DataDecrypted]


# ===== msg.DataDecryptedArray =====
class msg_dataDecryptedArray(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x09\x47\x5c\xe3"
    tl_type: typing.Literal["msg.dataDecryptedArray"] = pydantic.Field(
        alias="@type", default="msg.dataDecryptedArray"
    )
    elements: list["msg_DataDecrypted"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type msg_DataDecryptedArray = msg_dataDecryptedArray
msg_DataDecryptedArray_Model = pydantic.RootModel[msg_DataDecryptedArray]


# ===== msg.DataEncrypted =====
class msg_dataEncrypted(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x51\x3d\xa1\x21"
    tl_type: typing.Literal["msg.dataEncrypted"] = pydantic.Field(
        alias="@type", default="msg.dataEncrypted"
    )
    source: Option["AccountAddress"] = None
    data: Option["msg_Data"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type msg_DataEncrypted = msg_dataEncrypted
msg_DataEncrypted_Model = pydantic.RootModel[msg_DataEncrypted]


# ===== msg.DataEncryptedArray =====
class msg_dataEncryptedArray(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb2\x59\x47\x24"
    tl_type: typing.Literal["msg.dataEncryptedArray"] = pydantic.Field(
        alias="@type", default="msg.dataEncryptedArray"
    )
    elements: list["msg_DataEncrypted"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type msg_DataEncryptedArray = msg_dataEncryptedArray
msg_DataEncryptedArray_Model = pydantic.RootModel[msg_DataEncryptedArray]


# ===== msg.Message =====
class msg_message(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x16\xd1\x79\x9d"
    tl_type: typing.Literal["msg.message"] = pydantic.Field(alias="@type", default="msg.message")
    destination: Option["AccountAddress"] = None
    public_key: str = ""
    amount: int = 0
    extra_currencies: list["ExtraCurrency"] = []
    data: Option["msg_Data"] = None
    send_mode: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type msg_Message = msg_message
msg_Message_Model = pydantic.RootModel[msg_Message]


# ===== options.ConfigInfo =====
class options_configInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x16\x5f\xb7\x07"
    tl_type: typing.Literal["options.configInfo"] = pydantic.Field(
        alias="@type", default="options.configInfo"
    )
    default_wallet_id: int = 0
    default_rwallet_init_public_key: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type options_ConfigInfo = options_configInfo
options_ConfigInfo_Model = pydantic.RootModel[options_ConfigInfo]


# ===== options.Info =====
class options_info(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x80\x1c\x25\xfc"
    tl_type: typing.Literal["options.info"] = pydantic.Field(alias="@type", default="options.info")
    config_info: Option["options_ConfigInfo"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type options_Info = options_info
options_Info_Model = pydantic.RootModel[options_Info]


# ===== pchan.Action =====
class pchan_actionInit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8a\xf6\x2b\x1a"
    tl_type: typing.Literal["pchan.actionInit"] = pydantic.Field(
        alias="@type", default="pchan.actionInit"
    )
    inc_A: int = 0
    inc_B: int = 0
    min_A: int = 0
    min_B: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pchan_actionClose(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x16\x4b\x9c\x63"
    tl_type: typing.Literal["pchan.actionClose"] = pydantic.Field(
        alias="@type", default="pchan.actionClose"
    )
    extra_A: int = 0
    extra_B: int = 0
    promise: Option["pchan_Promise"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pchan_actionTimeout(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf3\x80\x1e\x77"
    tl_type: typing.Literal["pchan.actionTimeout"] = pydantic.Field(
        alias="@type", default="pchan.actionTimeout"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type pchan_Action = typing.Annotated[
    pchan_actionInit | pchan_actionClose | pchan_actionTimeout,
    pydantic.Field(discriminator="tl_type"),
]
pchan_Action_Model = pydantic.RootModel[pchan_Action]


# ===== pchan.Config =====
class pchan_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x36\xf4\x86\x84"
    tl_type: typing.Literal["pchan.config"] = pydantic.Field(alias="@type", default="pchan.config")
    alice_public_key: str = ""
    alice_address: Option["AccountAddress"] = None
    bob_public_key: str = ""
    bob_address: Option["AccountAddress"] = None
    init_timeout: int = 0
    close_timeout: int = 0
    channel_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type pchan_Config = pchan_config
pchan_Config_Model = pydantic.RootModel[pchan_Config]


# ===== pchan.Promise =====
class pchan_promise(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5d\x94\x0e\xa2"
    tl_type: typing.Literal["pchan.promise"] = pydantic.Field(
        alias="@type", default="pchan.promise"
    )
    signature: pydantic.Base64Bytes = b""
    promise_A: int = 0
    promise_B: int = 0
    channel_id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type pchan_Promise = pchan_promise
pchan_Promise_Model = pydantic.RootModel[pchan_Promise]


# ===== pchan.State =====
class pchan_stateInit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf8\x0c\x2a\xb9"
    tl_type: typing.Literal["pchan.stateInit"] = pydantic.Field(
        alias="@type", default="pchan.stateInit"
    )
    signed_A: bool = False
    signed_B: bool = False
    min_A: int = 0
    min_B: int = 0
    expire_at: int = 0
    A: int = 0
    B: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pchan_stateClose(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf3\x01\xe2\x34"
    tl_type: typing.Literal["pchan.stateClose"] = pydantic.Field(
        alias="@type", default="pchan.stateClose"
    )
    signed_A: bool = False
    signed_B: bool = False
    min_A: int = 0
    min_B: int = 0
    expire_at: int = 0
    A: int = 0
    B: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class pchan_statePayout(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x47\x14\x9e\x27"
    tl_type: typing.Literal["pchan.statePayout"] = pydantic.Field(
        alias="@type", default="pchan.statePayout"
    )
    A: int = 0
    B: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type pchan_State = typing.Annotated[
    pchan_stateInit | pchan_stateClose | pchan_statePayout, pydantic.Field(discriminator="tl_type")
]
pchan_State_Model = pydantic.RootModel[pchan_State]


# ===== query.Fees =====
class query_fees(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbe\x17\x3d\x60"
    tl_type: typing.Literal["query.fees"] = pydantic.Field(alias="@type", default="query.fees")
    source_fees: Option["Fees"] = None
    destination_fees: list["Fees"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type query_Fees = query_fees
query_Fees_Model = pydantic.RootModel[query_Fees]


# ===== query.Info =====
class query_info(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x70\xdc\x89\x56"
    tl_type: typing.Literal["query.info"] = pydantic.Field(alias="@type", default="query.info")
    id: int = 0
    valid_until: int = 0
    body_hash: pydantic.Base64Bytes = b""
    body: pydantic.Base64Bytes = b""
    init_state: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type query_Info = query_info
query_Info_Model = pydantic.RootModel[query_Info]


# ===== raw.ExtMessageInfo =====
class raw_extMessageInfo(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe3\x2f\xcf\x28"
    tl_type: typing.Literal["raw.extMessageInfo"] = pydantic.Field(
        alias="@type", default="raw.extMessageInfo"
    )
    hash: pydantic.Base64Bytes = b""
    hash_norm: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type raw_ExtMessageInfo = raw_extMessageInfo
raw_ExtMessageInfo_Model = pydantic.RootModel[raw_ExtMessageInfo]


# ===== raw.FullAccountState =====
class raw_fullAccountState(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x81\x10\x3e\xc4"
    tl_type: typing.Literal["raw.fullAccountState"] = pydantic.Field(
        alias="@type", default="raw.fullAccountState"
    )
    balance: int = 0
    extra_currencies: list["ExtraCurrency"] = []
    code: pydantic.Base64Bytes = b""
    data: pydantic.Base64Bytes = b""
    last_transaction_id: Option["internal_TransactionId"] = None
    block_id: Option["ton_BlockIdExt"] = None
    frozen_hash: pydantic.Base64Bytes = b""
    sync_utime: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type raw_FullAccountState = raw_fullAccountState
raw_FullAccountState_Model = pydantic.RootModel[raw_FullAccountState]


# ===== raw.Message =====
class raw_message(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3f\x70\x36\xe8"
    tl_type: typing.Literal["raw.message"] = pydantic.Field(alias="@type", default="raw.message")
    hash: pydantic.Base64Bytes = b""
    source: Option["AccountAddress"] = None
    destination: Option["AccountAddress"] = None
    value: int = 0
    extra_currencies: list["ExtraCurrency"] = []
    fwd_fee: int = 0
    ihr_fee: int = 0
    created_lt: int = 0
    body_hash: pydantic.Base64Bytes = b""
    msg_data: Option["msg_Data"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type raw_Message = raw_message
raw_Message_Model = pydantic.RootModel[raw_Message]


# ===== raw.Transaction =====
class raw_transaction(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x5a\x9f\x68\x8e"
    tl_type: typing.Literal["raw.transaction"] = pydantic.Field(
        alias="@type", default="raw.transaction"
    )
    address: Option["AccountAddress"] = None
    utime: int = 0
    data: pydantic.Base64Bytes = b""
    transaction_id: Option["internal_TransactionId"] = None
    fee: int = 0
    storage_fee: int = 0
    other_fee: int = 0
    in_msg: Option["raw_Message"] = None
    out_msgs: list["raw_Message"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type raw_Transaction = raw_transaction
raw_Transaction_Model = pydantic.RootModel[raw_Transaction]


# ===== raw.Transactions =====
class raw_transactions(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xed\xe8\xfa\x84"
    tl_type: typing.Literal["raw.transactions"] = pydantic.Field(
        alias="@type", default="raw.transactions"
    )
    transactions: list["raw_Transaction"] = []
    previous_transaction_id: Option["internal_TransactionId"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type raw_Transactions = raw_transactions
raw_Transactions_Model = pydantic.RootModel[raw_Transactions]


# ===== rwallet.Action =====
class rwallet_actionInit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x6b\xbd\x33\x25"
    tl_type: typing.Literal["rwallet.actionInit"] = pydantic.Field(
        alias="@type", default="rwallet.actionInit"
    )
    config: Option["rwallet_Config"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type rwallet_Action = rwallet_actionInit
rwallet_Action_Model = pydantic.RootModel[rwallet_Action]


# ===== rwallet.Config =====
class rwallet_config(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9a\x84\xe7\xfa"
    tl_type: typing.Literal["rwallet.config"] = pydantic.Field(
        alias="@type", default="rwallet.config"
    )
    start_at: int = 0
    limits: list["rwallet_Limit"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type rwallet_Config = rwallet_config
rwallet_Config_Model = pydantic.RootModel[rwallet_Config]


# ===== rwallet.Limit =====
class rwallet_limit(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x7e\xf6\xde\x48"
    tl_type: typing.Literal["rwallet.limit"] = pydantic.Field(
        alias="@type", default="rwallet.limit"
    )
    seconds: int = 0
    value: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type rwallet_Limit = rwallet_limit
rwallet_Limit_Model = pydantic.RootModel[rwallet_Limit]


# ===== smc.Info =====
class smc_info(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x3c\x96\x9b\x43"
    tl_type: typing.Literal["smc.info"] = pydantic.Field(alias="@type", default="smc.info")
    id: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_Info = smc_info
smc_Info_Model = pydantic.RootModel[smc_Info]


# ===== smc.LibraryEntry =====
class smc_libraryEntry(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x0c\xd2\xd5\xa3"
    tl_type: typing.Literal["smc.libraryEntry"] = pydantic.Field(
        alias="@type", default="smc.libraryEntry"
    )
    hash: Int256 = b"\x00" * 32
    data: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_LibraryEntry = smc_libraryEntry
smc_LibraryEntry_Model = pydantic.RootModel[smc_LibraryEntry]


# ===== smc.LibraryQueryExt =====
class smc_libraryQueryExt_one(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe3\xea\x24\x64"
    tl_type: typing.Literal["smc.libraryQueryExt.one"] = pydantic.Field(
        alias="@type", default="smc.libraryQueryExt.one"
    )
    hash: Int256 = b"\x00" * 32
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class smc_libraryQueryExt_scanBoc(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe5\xa4\x54\xdf"
    tl_type: typing.Literal["smc.libraryQueryExt.scanBoc"] = pydantic.Field(
        alias="@type", default="smc.libraryQueryExt.scanBoc"
    )
    boc: pydantic.Base64Bytes = b""
    max_libs: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_LibraryQueryExt = typing.Annotated[
    smc_libraryQueryExt_one | smc_libraryQueryExt_scanBoc, pydantic.Field(discriminator="tl_type")
]
smc_LibraryQueryExt_Model = pydantic.RootModel[smc_LibraryQueryExt]


# ===== smc.LibraryResult =====
class smc_libraryResult(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfe\xbb\x27\x0c"
    tl_type: typing.Literal["smc.libraryResult"] = pydantic.Field(
        alias="@type", default="smc.libraryResult"
    )
    result: list["smc_LibraryEntry"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_LibraryResult = smc_libraryResult
smc_LibraryResult_Model = pydantic.RootModel[smc_LibraryResult]


# ===== smc.LibraryResultExt =====
class smc_libraryResultExt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x72\xed\xde\xb4"
    tl_type: typing.Literal["smc.libraryResultExt"] = pydantic.Field(
        alias="@type", default="smc.libraryResultExt"
    )
    dict_boc: pydantic.Base64Bytes = b""
    libs_ok: list[Int256] = []
    libs_not_found: list[Int256] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_LibraryResultExt = smc_libraryResultExt
smc_LibraryResultExt_Model = pydantic.RootModel[smc_LibraryResultExt]


# ===== smc.MethodId =====
class smc_methodIdNumber(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xfc\xb9\x23\xa4"
    tl_type: typing.Literal["smc.methodIdNumber"] = pydantic.Field(
        alias="@type", default="smc.methodIdNumber"
    )
    number: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class smc_methodIdName(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x94\xff\x27\xf1"
    tl_type: typing.Literal["smc.methodIdName"] = pydantic.Field(
        alias="@type", default="smc.methodIdName"
    )
    name: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_MethodId = typing.Annotated[
    smc_methodIdNumber | smc_methodIdName, pydantic.Field(discriminator="tl_type")
]
smc_MethodId_Model = pydantic.RootModel[smc_MethodId]


# ===== smc.RunResult =====
class smc_runResult(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf3\xf3\x44\x54"
    tl_type: typing.Literal["smc.runResult"] = pydantic.Field(
        alias="@type", default="smc.runResult"
    )
    gas_used: int = 0
    stack: list["tvm_StackEntry"] = []
    exit_code: int = 0
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type smc_RunResult = smc_runResult
smc_RunResult_Model = pydantic.RootModel[smc_RunResult]


# ===== ton.BlockIdExt =====
class ton_blockIdExt(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x9a\xfc\x10\x79"
    tl_type: typing.Literal["ton.blockIdExt"] = pydantic.Field(
        alias="@type", default="ton.blockIdExt"
    )
    workchain: int = 0
    shard: int = 0
    seqno: int = 0
    root_hash: pydantic.Base64Bytes = b""
    file_hash: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type ton_BlockIdExt = ton_blockIdExt
ton_BlockIdExt_Model = pydantic.RootModel[ton_BlockIdExt]


# ===== tvm.Cell =====
class tvm_cell(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xa1\xa3\x5b\xe7"
    tl_type: typing.Literal["tvm.cell"] = pydantic.Field(alias="@type", default="tvm.cell")
    bytes: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tvm_Cell = tvm_cell
tvm_Cell_Model = pydantic.RootModel[tvm_Cell]


# ===== tvm.List =====
class tvm_list(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x08\x8d\xb7\x4b"
    tl_type: typing.Literal["tvm.list"] = pydantic.Field(alias="@type", default="tvm.list")
    elements: list["tvm_StackEntry"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tvm_List = tvm_list
tvm_List_Model = pydantic.RootModel[tvm_List]


# ===== tvm.Number =====
class tvm_numberDecimal(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xb3\x96\xe2\x45"
    tl_type: typing.Literal["tvm.numberDecimal"] = pydantic.Field(
        alias="@type", default="tvm.numberDecimal"
    )
    number: str = ""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tvm_Number = tvm_numberDecimal
tvm_Number_Model = pydantic.RootModel[tvm_Number]


# ===== tvm.Slice =====
class tvm_slice(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xe7\x8a\x06\x20"
    tl_type: typing.Literal["tvm.slice"] = pydantic.Field(alias="@type", default="tvm.slice")
    bytes: pydantic.Base64Bytes = b""
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tvm_Slice = tvm_slice
tvm_Slice_Model = pydantic.RootModel[tvm_Slice]


# ===== tvm.StackEntry =====
class tvm_stackEntrySlice(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x25\x6b\x2d\x53"
    tl_type: typing.Literal["tvm.stackEntrySlice"] = pydantic.Field(
        alias="@type", default="tvm.stackEntrySlice"
    )
    slice: Option["tvm_Slice"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tvm_stackEntryCell(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x20\x6f\xb1\x4d"
    tl_type: typing.Literal["tvm.stackEntryCell"] = pydantic.Field(
        alias="@type", default="tvm.stackEntryCell"
    )
    cell: Option["tvm_Cell"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tvm_stackEntryNumber(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xbe\x3d\xfb\x50"
    tl_type: typing.Literal["tvm.stackEntryNumber"] = pydantic.Field(
        alias="@type", default="tvm.stackEntryNumber"
    )
    number: Option["tvm_Number"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tvm_stackEntryTuple(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xdc\x63\x9e\xf6"
    tl_type: typing.Literal["tvm.stackEntryTuple"] = pydantic.Field(
        alias="@type", default="tvm.stackEntryTuple"
    )
    tuple: Option["tvm_Tuple"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tvm_stackEntryList(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x8b\x2d\x44\xb9"
    tl_type: typing.Literal["tvm.stackEntryList"] = pydantic.Field(
        alias="@type", default="tvm.stackEntryList"
    )
    list_: Option["tvm_List"] = None
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


class tvm_stackEntryUnsupported(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\xf2\x41\x95\x16"
    tl_type: typing.Literal["tvm.stackEntryUnsupported"] = pydantic.Field(
        alias="@type", default="tvm.stackEntryUnsupported"
    )
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tvm_StackEntry = typing.Annotated[
    tvm_stackEntrySlice
    | tvm_stackEntryCell
    | tvm_stackEntryNumber
    | tvm_stackEntryTuple
    | tvm_stackEntryList
    | tvm_stackEntryUnsupported,
    pydantic.Field(discriminator="tl_type"),
]
tvm_StackEntry_Model = pydantic.RootModel[tvm_StackEntry]


# ===== tvm.Tuple =====
class tvm_tuple(pydantic.BaseModel):
    tl_tag: typing.ClassVar[bytes] = b"\x63\xba\xb3\xae"
    tl_type: typing.Literal["tvm.tuple"] = pydantic.Field(alias="@type", default="tvm.tuple")
    elements: list["tvm_StackEntry"] = []
    model_config: typing.ClassVar[pydantic.ConfigDict] = pydantic.ConfigDict(
        serialize_by_alias=True, strict=True
    )


type tvm_Tuple = tvm_tuple
tvm_Tuple_Model = pydantic.RootModel[tvm_Tuple]
