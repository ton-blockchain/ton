import base64
import json

import pytest
from tontester.tl.ton_api import (
    Adnl_packetContents,
    Catchain_config_global,
    Engine_validator_config,
    Id_config_local,
    Pk_aes,
    Pub_unenc,
    TonNode_blockIdExt,
)


@pytest.fixture()
def pubk():
    return Pub_unenc(data=b"\x12" * 32)


@pytest.fixture()
def block():
    return TonNode_blockIdExt(
        workchain=-1, shard=0, seqno=123456, root_hash=b"\x01" * 32, file_hash=b"\x02" * 32
    )


def test_reading_engine_validator_config():
    config = """
        {
           "@type" : "engine.validator.config",
           "out_port" : 3278,
           "addrs" : [
              {
                 "@type" : "engine.addr",
                 "ip" : -1795443737,
                 "port" : 57227,
                 "categories" : [
                    0,
                    1,
                    2,
                    3
                 ],
                 "priority_categories" : [
                 ]
              }
           ],
           "adnl" : [
              {
                 "@type" : "engine.adnl",
                 "id" : "Ube8wLWXM3VOCsIaR9e2vojDMTxjpVRo6De1SJQoKM4=",
                 "category" : 0
              },
              {
                 "@type" : "engine.adnl",
                 "id" : "0XjyWTqN0+Q7CxboL/ag9gMfvGLOiZOFFA8+nQSv7IM=",
                 "category" : 0
              },
              {
                 "@type" : "engine.adnl",
                 "id" : "4VuLuYl0uqw87kQH+549L/JQwiOOWCOBbwMayqhX2Jo=",
                 "category" : 1
              }
           ],
           "dht" : [
              {
                 "@type" : "engine.dht",
                 "id" : "0XjyWTqN0+Q7CxboL/ag9gMfvGLOiZOFFA8+nQSv7IM="
              }
           ],
           "validators" : [
           ],
           "collators" : [
           ],
           "fullnode" : "4VuLuYl0uqw87kQH+549L/JQwiOOWCOBbwMayqhX2Jo=",
           "fullnodeslaves" : [
           ],
           "fullnodemasters" : [
           ],
           "liteservers" : [
              {
                 "@type" : "engine.liteServer",
                 "id" : "LOQ9rpOR3RSYGCiRRKcOvlN3n31kIW6qv8M7XP07eZM=",
                 "port" : 10374
              }
           ],
           "control" : [
              {
                 "@type" : "engine.controlInterface",
                 "id" : "mhwOm4a0goKT8wVlhVyNQGHkqsg2FzTX1YK3Zp9NV08=",
                 "port" : 20223,
                 "allowed" : [
                    {
                       "@type" : "engine.controlProcess",
                       "id" : "NhRlwaHf4OdLtuoOPm6p1yZmAIH8o+2/z7XqRmVnwv4=",
                       "permissions" : 15
                    }
                 ]
              }
           ],
           "shards_to_monitor" : [
           ],
           "gc" : {
              "@type" : "engine.gc",
              "ids" : [
              ]
           }
        }
    """

    vc = Engine_validator_config.from_json(config)

    # bytes should be deserialized from base64 string
    assert vc.adnl is not None
    adnl_entry = vc.adnl[0]
    assert adnl_entry is not None
    adnl_id = adnl_entry.id
    assert adnl_id is not None
    assert adnl_id == base64.b64decode("Ube8wLWXM3VOCsIaR9e2vojDMTxjpVRo6De1SJQoKM4=")

    # serialization back to json should be the same
    assert vc.to_json() == json.dumps(json.loads(config))


def test_to_dict(pubk: Pub_unenc, block: TonNode_blockIdExt):
    assert block.to_dict() == {
        "@type": "tonNode.blockIdExt",
        "workchain": -1,
        "shard": 0,
        "seqno": 123456,
        "root_hash": "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=",
        "file_hash": "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI=",
    }

    cg = Catchain_config_global(tag=b"\04" * 32, nodes=[pubk, pubk])
    assert cg.to_dict() == {
        "@type": "catchain.config.global",
        "tag": "BAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ=",
        "nodes": [
            {"@type": "pub.unenc", "data": "EhISEhISEhISEhISEhISEhISEhISEhISEhISEhISEhI="},
            {"@type": "pub.unenc", "data": "EhISEhISEhISEhISEhISEhISEhISEhISEhISEhISEhI="},
        ],
    }

    pc = Adnl_packetContents(rand1=b"\x01\x01", flags=1, rand2=b"\x01\x01", from_=pubk)
    assert pc.to_dict() == {
        "@type": "adnl.packetContents",
        "rand1": "AQE=",
        "flags": 1,
        "from_": {"@type": "pub.unenc", "data": "EhISEhISEhISEhISEhISEhISEhISEhISEhISEhISEhI="},
        "messages": [],
        "seqno": 0,
        "confirm_seqno": 0,
        "recv_addr_list_version": 0,
        "recv_priority_addr_list_version": 0,
        "reinit_date": 0,
        "dst_reinit_date": 0,
        "signature": "",
        "rand2": "AQE=",
    }
    pc2 = Adnl_packetContents.from_dict(pc.to_dict())
    assert pc == pc2


def test_from_dict(pubk: Pub_unenc):
    key = b"\x01" * 32
    c = Id_config_local(id=Pk_aes(key=key))

    c2 = Id_config_local.from_dict(
        {
            "@type": "id.config.local",
            "id": {
                "@type": "pk.aes",
                "key": base64.b64encode(key).decode(),
            },
        }
    )
    assert c == c2

    pc_decoded = Adnl_packetContents.from_dict(
        {
            "@type": "adnl.packetContents",
            "rand1": "AQE=",
            "flags": 1,
            "from_": {"@type": "pub.unenc", "data": "EhISEhISEhISEhISEhISEhISEhISEhISEhISEhISEhI="},
            "rand2": "AQE=",
        }
    )
    pc_expected = Adnl_packetContents(rand1=b"\x01\x01", flags=1, rand2=b"\x01\x01", from_=pubk)
    assert pc_decoded == pc_expected
