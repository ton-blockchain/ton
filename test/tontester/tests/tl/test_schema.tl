simpleObject id:int name:string = SimpleObject;

variantA value:int = TestVariant;
variantB text:string = TestVariant;
variantC flag:Bool = TestVariant;

allPrimitives
  field_int:int
  field_long:long
  field_int32:int32
  field_int53:int53
  field_int64:int64
  field_int128:int128
  field_int256:int256
  field_double:double
  field_string:string
  field_bytes:bytes
  field_Bool:Bool
  field_secureString:secureString
  field_secureBytes:secureBytes
  = AllPrimitives;

vectorPrimitives
  vec_int:vector<int>
  vec_long:vector<long>
  vec_int32:vector<int32>
  vec_int53:vector<int53>
  vec_int64:vector<int64>
  vec_int128:vector<int128>
  vec_int256:vector<int256>
  vec_double:vector<double>
  vec_string:vector<string>
  vec_bytes:vector<bytes>
  vec_Bool:vector<Bool>
  vec_secureString:vector<secureString>
  vec_secureBytes:vector<secureBytes>
  = VectorPrimitives;

nestedObject
  simple:SimpleObject
  variant:TestVariant
  = NestedObject;

vectorObjects
  simples:vector<SimpleObject>
  variants:vector<TestVariant>
  = VectorObjects;

optionalFields
  flags:#
  opt_int:flags.0?int
  opt_string:flags.1?string
  opt_object:flags.2?SimpleObject
  opt_vec:flags.3?vector<int>
  = OptionalFields;

trueField has_feature:true other:int = TrueField;

---functions---

query id:int data:string = AllPrimitives;

objectQuery obj:SimpleObject = NestedObject;
