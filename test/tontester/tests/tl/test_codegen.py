import base64

from generated.test_schema import (
    AllPrimitives,
    NestedObject,
    ObjectQueryRequest,
    OptionalFields,
    QueryRequest,
    SimpleObject,
    TrueField,
    VariantA,
    VariantB,
    VariantC,
    VectorObjects,
    VectorPrimitives,
)


def test_all_primitives_serialize():
    obj = AllPrimitives(
        field_int=42,
        field_long=9223372036854775807,
        field_int32=-2147483648,
        field_int53=4503599627370495,
        field_int64=-9223372036854775808,
        field_int128=b"\x01" * 16,
        field_int256=b"\x02" * 32,
        field_double=3.14159,
        field_string="test string",
        field_bytes=b"test bytes",
        field_Bool=True,
        field_secureString="secure",
        field_secureBytes=b"secure bytes",
    )

    expected = {
        "@type": "allPrimitives",
        "field_int": 42,
        "field_long": 9223372036854775807,
        "field_int32": -2147483648,
        "field_int53": 4503599627370495,
        "field_int64": -9223372036854775808,
        "field_int128": base64.b64encode(b"\x01" * 16).decode(),
        "field_int256": base64.b64encode(b"\x02" * 32).decode(),
        "field_double": 3.14159,
        "field_string": "test string",
        "field_bytes": base64.b64encode(b"test bytes").decode(),
        "field_Bool": True,
        "field_secureString": "secure",
        "field_secureBytes": base64.b64encode(b"secure bytes").decode(),
    }

    assert obj.to_dict() == expected


def test_all_primitives_deserialize():
    d = {
        "@type": "allPrimitives",
        "field_int": 42,
        "field_long": 9223372036854775807,
        "field_int32": -2147483648,
        "field_int53": 4503599627370495,
        "field_int64": -9223372036854775808,
        "field_int128": base64.b64encode(b"\x01" * 16).decode(),
        "field_int256": base64.b64encode(b"\x02" * 32).decode(),
        "field_double": 3.14159,
        "field_string": "test string",
        "field_bytes": base64.b64encode(b"test bytes").decode(),
        "field_Bool": True,
        "field_secureString": "secure",
        "field_secureBytes": base64.b64encode(b"secure bytes").decode(),
    }

    obj = AllPrimitives.from_dict(d)

    assert obj.field_int == 42
    assert obj.field_long == 9223372036854775807
    assert obj.field_int32 == -2147483648
    assert obj.field_int53 == 4503599627370495
    assert obj.field_int64 == -9223372036854775808
    assert obj.field_int128 == b"\x01" * 16
    assert obj.field_int256 == b"\x02" * 32
    assert obj.field_double == 3.14159
    assert obj.field_string == "test string"
    assert obj.field_bytes == b"test bytes"
    assert obj.field_Bool is True
    assert obj.field_secureString == "secure"
    assert obj.field_secureBytes == b"secure bytes"


def test_all_primitives_roundtrip():
    obj = AllPrimitives(
        field_int=0,
        field_long=0,
        field_int32=0,
        field_int53=0,
        field_int64=0,
        field_int128=b"\x00" * 16,
        field_int256=b"\xff" * 32,
        field_double=0.0,
        field_string="",
        field_bytes=b"",
        field_Bool=False,
        field_secureString="",
        field_secureBytes=b"",
    )

    obj2 = AllPrimitives.from_dict(obj.to_dict())
    assert obj == obj2


def test_all_primitives_defaults():
    d = {"@type": "allPrimitives"}

    obj = AllPrimitives.from_dict(d)

    assert obj.field_int == 0
    assert obj.field_long == 0
    assert obj.field_int32 == 0
    assert obj.field_int53 == 0
    assert obj.field_int64 == 0
    assert obj.field_int128 == b"\x00" * 16
    assert obj.field_int256 == b"\x00" * 32
    assert obj.field_double == 0.0
    assert obj.field_string == ""
    assert obj.field_bytes == b""
    assert obj.field_Bool is False
    assert obj.field_secureString == ""
    assert obj.field_secureBytes == b""


def test_vector_primitives_serialize():
    obj = VectorPrimitives(
        vec_int=[1, 2, 3],
        vec_long=[100, 200, 300],
        vec_int32=[-1, -2, -3],
        vec_int53=[1000, 2000, 3000],
        vec_int64=[10000, 20000, 30000],
        vec_int128=[b"\x01" * 16, b"\x02" * 16],
        vec_int256=[b"\x03" * 32, b"\x04" * 32],
        vec_double=[1.1, 2.2, 3.3],
        vec_string=["a", "b", "c"],
        vec_bytes=[b"x", b"y", b"z"],
        vec_Bool=[True, False, True],
        vec_secureString=["s1", "s2"],
        vec_secureBytes=[b"sb1", b"sb2"],
    )

    expected = {
        "@type": "vectorPrimitives",
        "vec_int": [1, 2, 3],
        "vec_long": [100, 200, 300],
        "vec_int32": [-1, -2, -3],
        "vec_int53": [1000, 2000, 3000],
        "vec_int64": [10000, 20000, 30000],
        "vec_int128": [
            base64.b64encode(b"\x01" * 16).decode(),
            base64.b64encode(b"\x02" * 16).decode(),
        ],
        "vec_int256": [
            base64.b64encode(b"\x03" * 32).decode(),
            base64.b64encode(b"\x04" * 32).decode(),
        ],
        "vec_double": [1.1, 2.2, 3.3],
        "vec_string": ["a", "b", "c"],
        "vec_bytes": [
            base64.b64encode(b"x").decode(),
            base64.b64encode(b"y").decode(),
            base64.b64encode(b"z").decode(),
        ],
        "vec_Bool": [True, False, True],
        "vec_secureString": ["s1", "s2"],
        "vec_secureBytes": [base64.b64encode(b"sb1").decode(), base64.b64encode(b"sb2").decode()],
    }

    assert obj.to_dict() == expected


def test_vector_primitives_deserialize():
    d = {
        "@type": "vectorPrimitives",
        "vec_int": [1, 2, 3],
        "vec_long": [100, 200, 300],
        "vec_int32": [-1, -2, -3],
        "vec_int53": [1000, 2000, 3000],
        "vec_int64": [10000, 20000, 30000],
        "vec_int128": [
            base64.b64encode(b"\x01" * 16).decode(),
            base64.b64encode(b"\x02" * 16).decode(),
        ],
        "vec_int256": [
            base64.b64encode(b"\x03" * 32).decode(),
            base64.b64encode(b"\x04" * 32).decode(),
        ],
        "vec_double": [1.1, 2.2, 3.3],
        "vec_string": ["a", "b", "c"],
        "vec_bytes": [
            base64.b64encode(b"x").decode(),
            base64.b64encode(b"y").decode(),
            base64.b64encode(b"z").decode(),
        ],
        "vec_Bool": [True, False, True],
        "vec_secureString": ["s1", "s2"],
        "vec_secureBytes": [base64.b64encode(b"sb1").decode(), base64.b64encode(b"sb2").decode()],
    }

    obj = VectorPrimitives.from_dict(d)

    assert obj.vec_int == [1, 2, 3]
    assert obj.vec_long == [100, 200, 300]
    assert obj.vec_int32 == [-1, -2, -3]
    assert obj.vec_int53 == [1000, 2000, 3000]
    assert obj.vec_int64 == [10000, 20000, 30000]
    assert obj.vec_int128 == [b"\x01" * 16, b"\x02" * 16]
    assert obj.vec_int256 == [b"\x03" * 32, b"\x04" * 32]
    assert obj.vec_double == [1.1, 2.2, 3.3]
    assert obj.vec_string == ["a", "b", "c"]
    assert obj.vec_bytes == [b"x", b"y", b"z"]
    assert obj.vec_Bool == [True, False, True]
    assert obj.vec_secureString == ["s1", "s2"]
    assert obj.vec_secureBytes == [b"sb1", b"sb2"]


def test_vector_primitives_empty():
    obj = VectorPrimitives(
        vec_int=[],
        vec_long=[],
        vec_int32=[],
        vec_int53=[],
        vec_int64=[],
        vec_int128=[],
        vec_int256=[],
        vec_double=[],
        vec_string=[],
        vec_bytes=[],
        vec_Bool=[],
        vec_secureString=[],
        vec_secureBytes=[],
    )

    obj2 = VectorPrimitives.from_dict(obj.to_dict())
    assert obj == obj2


def test_vector_primitives_defaults():
    d = {"@type": "vectorPrimitives"}

    obj = VectorPrimitives.from_dict(d)

    assert obj.vec_int == []
    assert obj.vec_long == []
    assert obj.vec_int32 == []
    assert obj.vec_int53 == []
    assert obj.vec_int64 == []
    assert obj.vec_int128 == []
    assert obj.vec_int256 == []
    assert obj.vec_double == []
    assert obj.vec_string == []
    assert obj.vec_bytes == []
    assert obj.vec_Bool == []
    assert obj.vec_secureString == []
    assert obj.vec_secureBytes == []


def test_simple_object():
    obj = SimpleObject(id=123, name="test")

    expected = {"@type": "simpleObject", "id": 123, "name": "test"}

    assert obj.to_dict() == expected

    obj2 = SimpleObject.from_dict(expected)
    assert obj == obj2


def test_simple_object_defaults():
    d = {"@type": "simpleObject"}

    obj = SimpleObject.from_dict(d)

    assert obj.id == 0
    assert obj.name == ""


def test_variant_a():
    obj = VariantA(value=42)

    expected = {"@type": "variantA", "value": 42}

    assert obj.to_dict() == expected

    obj2 = VariantA.from_dict(expected)
    assert obj == obj2


def test_variant_b():
    obj = VariantB(text="hello")

    expected = {"@type": "variantB", "text": "hello"}

    assert obj.to_dict() == expected

    obj2 = VariantB.from_dict(expected)
    assert obj == obj2


def test_variant_c():
    obj = VariantC(flag=False)

    expected = {"@type": "variantC", "flag": False}

    assert obj.to_dict() == expected

    obj2 = VariantC.from_dict(expected)
    assert obj == obj2


def test_nested_object_with_variant_a():
    simple = SimpleObject(id=1, name="obj1")
    variant = VariantA(value=100)
    obj = NestedObject(simple=simple, variant=variant)

    expected = {
        "@type": "nestedObject",
        "simple": {"@type": "simpleObject", "id": 1, "name": "obj1"},
        "variant": {"@type": "variantA", "value": 100},
    }

    assert obj.to_dict() == expected

    obj2 = NestedObject.from_dict(expected)
    assert obj == obj2


def test_nested_object_with_variant_b():
    simple = SimpleObject(id=2, name="obj2")
    variant = VariantB(text="variant b text")
    obj = NestedObject(simple=simple, variant=variant)

    obj2 = NestedObject.from_dict(obj.to_dict())
    assert obj == obj2


def test_nested_object_defaults():
    d = {"@type": "nestedObject"}

    obj = NestedObject.from_dict(d)

    assert obj.simple is None
    assert obj.variant is None


def test_vector_objects():
    obj = VectorObjects(
        simples=[
            SimpleObject(id=1, name="first"),
            SimpleObject(id=2, name="second"),
        ],
        variants=[
            VariantA(value=10),
            VariantB(text="text"),
            VariantC(flag=True),
        ],
    )

    expected = {
        "@type": "vectorObjects",
        "simples": [
            {"@type": "simpleObject", "id": 1, "name": "first"},
            {"@type": "simpleObject", "id": 2, "name": "second"},
        ],
        "variants": [
            {"@type": "variantA", "value": 10},
            {"@type": "variantB", "text": "text"},
            {"@type": "variantC", "flag": True},
        ],
    }

    assert obj.to_dict() == expected

    obj2 = VectorObjects.from_dict(expected)
    assert obj == obj2


def test_vector_objects_defaults():
    d = {"@type": "vectorObjects"}

    obj = VectorObjects.from_dict(d)

    assert obj.simples == []
    assert obj.variants == []


def test_optional_fields_all_present():
    obj = OptionalFields(
        flags=15,
        opt_int=123,
        opt_string="present",
        opt_object=SimpleObject(id=1, name="obj"),
        opt_vec=[10, 20, 30],
    )

    expected = {
        "@type": "optionalFields",
        "flags": 15,
        "opt_int": 123,
        "opt_string": "present",
        "opt_object": {"@type": "simpleObject", "id": 1, "name": "obj"},
        "opt_vec": [10, 20, 30],
    }

    assert obj.to_dict() == expected

    obj2 = OptionalFields.from_dict(expected)
    assert obj == obj2


def test_optional_fields_only_flags():
    obj = OptionalFields(flags=7, opt_int=0, opt_string="", opt_object=None, opt_vec=None)

    d = obj.to_dict()
    obj2 = OptionalFields.from_dict(d)
    assert obj == obj2


def test_optional_fields_flags_mismatch():
    d = {
        "@type": "optionalFields",
        "flags": 0,
        "opt_int": 999,
        "opt_string": "present even though flag is 0",
    }

    obj = OptionalFields.from_dict(d)

    assert obj.flags == 0
    assert obj.opt_int == 999
    assert obj.opt_string == "present even though flag is 0"
    assert obj.opt_object is None
    assert obj.opt_vec == []


def test_optional_fields_defaults():
    d = {"@type": "optionalFields"}

    obj = OptionalFields.from_dict(d)

    assert obj.flags == 0
    assert obj.opt_int == 0
    assert obj.opt_string == ""
    assert obj.opt_object is None
    assert obj.opt_vec == []


def test_true_field():
    obj = TrueField(has_feature=True, other=456)

    expected = {"@type": "trueField", "has_feature": True, "other": 456}

    assert obj.to_dict() == expected

    obj2 = TrueField.from_dict(expected)
    assert obj == obj2


def test_true_field_defaults():
    d = {"@type": "trueField"}

    obj = TrueField.from_dict(d)

    assert obj.has_feature is True
    assert obj.other == 0


def test_request():
    req = QueryRequest(id=789, data="request data")

    expected = {"@type": "query", "id": 789, "data": "request data"}

    assert req.to_dict() == expected

    req2 = QueryRequest.from_dict(expected)
    assert req == req2


def test_object_request():
    obj = SimpleObject(id=999, name="in request")
    req = ObjectQueryRequest(obj=obj)

    expected = {
        "@type": "objectQuery",
        "obj": {"@type": "simpleObject", "id": 999, "name": "in request"},
    }

    assert req.to_dict() == expected

    req2 = ObjectQueryRequest.from_dict(expected)
    assert req == req2


def test_object_request_defaults():
    d = {"@type": "objectQuery"}

    req = ObjectQueryRequest.from_dict(d)

    assert req.obj is None
