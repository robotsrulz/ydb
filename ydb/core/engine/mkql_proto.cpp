#include "mkql_proto.h"

#include <ydb/library/yql/minikql/defs.h>
#include <ydb/library/yql/minikql/mkql_node_visitor.h>
#include <ydb/library/yql/minikql/mkql_string_util.h>
#include <ydb/library/yql/minikql/computation/mkql_computation_node.h>
#include <ydb/library/yql/minikql/computation/mkql_computation_node_holders.h>
#include <ydb/library/yql/public/decimal/yql_decimal.h>

#include <ydb/core/scheme_types/scheme_types_defs.h>

namespace NKikimr {

namespace NMiniKQL {

namespace {

    Y_FORCE_INLINE NUdf::TUnboxedValue HandleKindDataImport(TType* type, const Ydb::Value& value) {
        auto dataType = static_cast<TDataType*>(type);
        switch (dataType->GetSchemeType()) {
            case NUdf::TDataType<bool>::Id:
                return NUdf::TUnboxedValuePod(value.bool_value());
            case NUdf::TDataType<ui8>::Id:
                return NUdf::TUnboxedValuePod(ui8(value.uint32_value()));
            case NUdf::TDataType<i8>::Id:
                return NUdf::TUnboxedValuePod(i8(value.int32_value()));
            case NUdf::TDataType<ui16>::Id:
                return NUdf::TUnboxedValuePod(ui16(value.uint32_value()));
            case NUdf::TDataType<i16>::Id:
                return NUdf::TUnboxedValuePod(i16(value.int32_value()));
            case NUdf::TDataType<i32>::Id:
                return NUdf::TUnboxedValuePod(value.int32_value());
            case NUdf::TDataType<ui32>::Id:
                return NUdf::TUnboxedValuePod(value.uint32_value());
            case NUdf::TDataType<i64>::Id:
                return NUdf::TUnboxedValuePod(value.int64_value());
            case NUdf::TDataType<ui64>::Id:
                return NUdf::TUnboxedValuePod(value.uint64_value());
            case NUdf::TDataType<float>::Id:
                return NUdf::TUnboxedValuePod(value.float_value());
            case NUdf::TDataType<double>::Id:
                return NUdf::TUnboxedValuePod(value.double_value());
            case NUdf::TDataType<NUdf::TJson>::Id:
            case NUdf::TDataType<NUdf::TUtf8>::Id:
                return MakeString(value.text_value());
            case NUdf::TDataType<NUdf::TDate>::Id:
                return NUdf::TUnboxedValuePod(ui16(value.uint32_value()));
            case NUdf::TDataType<NUdf::TDatetime>::Id:
                return NUdf::TUnboxedValuePod(value.uint32_value());
            case NUdf::TDataType<NUdf::TTimestamp>::Id:
                return NUdf::TUnboxedValuePod(value.uint64_value());
            case NUdf::TDataType<NUdf::TInterval>::Id:
                return NUdf::TUnboxedValuePod(value.int64_value());
            case NUdf::TDataType<NUdf::TJsonDocument>::Id:
                return ValueFromString(NUdf::EDataSlot::JsonDocument, value.text_value());
            case NUdf::TDataType<NUdf::TDyNumber>::Id:
                return ValueFromString(NUdf::EDataSlot::DyNumber, value.text_value());
            case NUdf::TDataType<NUdf::TDecimal>::Id:
                return NUdf::TUnboxedValuePod(NYql::NDecimal::FromHalfs(value.low_128(), value.high_128()));
            default:
                return MakeString(value.bytes_value());
        }
    }

}

NUdf::TUnboxedValue ImportValueFromProto(TType* type, const Ydb::Value& value, const THolderFactory& factory) {
    switch (type->GetKind()) {
    case TType::EKind::Void:
        return NUdf::TUnboxedValuePod::Void();

    case TType::EKind::Null:
    case TType::EKind::EmptyList:
    case TType::EKind::EmptyDict:
        return NUdf::TUnboxedValue();

    case TType::EKind::Data:
        return HandleKindDataImport(type, value);

    case TType::EKind::Optional: {
        auto optionalType = static_cast<TOptionalType*>(type);
        switch (value.value_case()) {
            case Ydb::Value::kNestedValue:
                return ImportValueFromProto(optionalType->GetItemType(), value.nested_value(), factory).MakeOptional();
            case Ydb::Value::kNullFlagValue:
                return NUdf::TUnboxedValue();
            default:
                return ImportValueFromProto(optionalType->GetItemType(), value, factory).MakeOptional();
        }
    }

    case TType::EKind::List: {
        auto listType = static_cast<TListType*>(type);
        auto itemType = listType->GetItemType();
        const auto& list = value.items();
        NUdf::TUnboxedValue *items = nullptr;
        auto array = factory.CreateDirectArrayHolder(list.size(), items);
        for (const auto& x : list) {
            *items++ = ImportValueFromProto(itemType, x, factory);
        }

        return std::move(array);
    }

    case TType::EKind::Struct: {
        auto structType = static_cast<TStructType*>(type);
        NUdf::TUnboxedValue* itemsPtr = nullptr;
        auto res = factory.CreateDirectArrayHolder(structType->GetMembersCount(), itemsPtr);
        for (ui32 index = 0; index < structType->GetMembersCount(); ++index) {
            auto memberType = structType->GetMemberType(index);
            itemsPtr[index] = ImportValueFromProto(memberType, value.items(index), factory);
        }

        return std::move(res);
    }

    case TType::EKind::Tuple: {
        auto tupleType = static_cast<TTupleType*>(type);
        NUdf::TUnboxedValue* itemsPtr = nullptr;
        auto res = factory.CreateDirectArrayHolder(tupleType->GetElementsCount(), itemsPtr);
        for (ui32 index = 0; index < tupleType->GetElementsCount(); ++index) {
            auto elementType = tupleType->GetElementType(index);
            itemsPtr[index] = ImportValueFromProto(elementType, value.items(index), factory);
        }

        return std::move(res);
    }

    case TType::EKind::Dict: {
        auto dictType = static_cast<TDictType*>(type);
        auto keyType = dictType->GetKeyType();
        auto payloadType = dictType->GetPayloadType();
        auto dictBuilder = factory.NewDict(dictType, NUdf::TDictFlags::EDictKind::Hashed);

        for (const auto& x : value.pairs()) {
            dictBuilder->Add(
                ImportValueFromProto(keyType, x.key(), factory),
                ImportValueFromProto(payloadType, x.payload(), factory)
            );
        }

        return dictBuilder->Build();
    }

    case TType::EKind::Variant: {
        auto variantType = static_cast<TVariantType*>(type);
        auto index = value.variant_index();
        auto unboxedValue = ImportValueFromProto(variantType->GetAlternativeType(index), value, factory);
        auto res = factory.CreateVariantHolder(std::move(unboxedValue.Release()), index);
        return std::move(res);
    }

    case TType::EKind::Tagged: {
        auto taggedType = static_cast<TTaggedType*>(type);
        auto unboxedValue = ImportValueFromProto(taggedType->GetBaseType(), value, factory);
        return std::move(unboxedValue);
    }

    default:
        MKQL_ENSURE(false, TStringBuilder() << "Unknown kind: " << type->GetKindAsStr());
    }
}


template <typename ValType>
class TAlmostDoneTypeValue : public TRawTypeValue {
public:
    TAlmostDoneTypeValue(NUdf::TDataTypeId schemeType, ValType value)
        : TRawTypeValue(&Value, sizeof(Value), schemeType)
        , Value(value)
    {}

protected:
    ValType Value;
};

template <>
class TAlmostDoneTypeValue<TString> : public TRawTypeValue {
public:
    TAlmostDoneTypeValue(NUdf::TDataTypeId schemeType, const TString& value)
        : TRawTypeValue(value.data(), value.size(), schemeType)
    {}
};

// NOTE: TCell's can reference memomry from tupleValue
bool CellsFromTuple(const NKikimrMiniKQL::TType* tupleType,
                    const NKikimrMiniKQL::TValue& tupleValue,
                    const TConstArrayRef<NScheme::TTypeId>& types,
                    bool allowCastFromString,
                    TVector<TCell>& key,
                    TString& errStr)
{

#define CHECK_OR_RETURN_ERROR(cond, descr) \
    if (!(cond)) { \
        errStr = descr; \
        return false; \
    }

    if (tupleType) {
        CHECK_OR_RETURN_ERROR(tupleType->GetKind() == NKikimrMiniKQL::Tuple ||
                              (tupleType->GetKind() == NKikimrMiniKQL::Unknown && tupleType->GetTuple().ElementSize() == 0), "Must be a tuple");
        CHECK_OR_RETURN_ERROR(tupleType->GetTuple().ElementSize() <= types.size(),
            "Tuple size " + ToString(tupleType->GetTuple().ElementSize()) + " is greater that expected size " + ToString(types.size()));

        for (size_t i = 0; i < tupleType->GetTuple().ElementSize(); ++i) {
            const auto& ti = tupleType->GetTuple().GetElement(i);
            CHECK_OR_RETURN_ERROR(ti.GetKind() == NKikimrMiniKQL::Optional, "Element at index " + ToString(i) + " in not an Optional");
            const auto& item = ti.GetOptional().GetItem();
            CHECK_OR_RETURN_ERROR(item.GetKind() == NKikimrMiniKQL::Data, "Element at index " + ToString(i) + " Item kind is not Data");
            const auto& typeId = item.GetData().GetScheme();
            CHECK_OR_RETURN_ERROR(typeId == types[i] ||
                allowCastFromString && (typeId == NScheme::NTypeIds::Utf8),
                "Element at index " + ToString(i) + " has type " + ToString(typeId) + " but expected type is " + ToString(types[i]));
        }

        CHECK_OR_RETURN_ERROR(tupleType->GetTuple().ElementSize() == tupleValue.TupleSize(),
            Sprintf("Tuple value length %" PRISZT " doesn't match the length in type %" PRISZT, tupleValue.TupleSize(), tupleType->GetTuple().ElementSize()));
    } else {
        CHECK_OR_RETURN_ERROR(types.size() >= tupleValue.TupleSize(),
            Sprintf("Tuple length %" PRISZT " is greater than key column count %" PRISZT, tupleValue.TupleSize(), types.size()));
    }

    for (ui32 i = 0; i < tupleValue.TupleSize(); ++i) {
        auto& o = tupleValue.GetTuple(i);

        auto element_case = o.value_value_case();

        CHECK_OR_RETURN_ERROR(element_case == NKikimrMiniKQL::TValue::kOptional ||
                              element_case == NKikimrMiniKQL::TValue::VALUE_VALUE_NOT_SET,
                              Sprintf("Optional type is expected in tuple at position %" PRIu32, i));

        CHECK_OR_RETURN_ERROR(o.ListSize() == 0 &&
                              o.StructSize() == 0 &&
                              o.TupleSize() == 0 &&
                              o.DictSize() == 0,
                              Sprintf("Optional type is expected in tuple at position %" PRIu32, i));

        if (!o.HasOptional()) {
            key.push_back(TCell());
            continue;
        }

        auto& v = o.GetOptional();

        auto value_case = v.value_value_case();

        CHECK_OR_RETURN_ERROR(value_case != NKikimrMiniKQL::TValue::kOptional &&
                              value_case != NKikimrMiniKQL::TValue::VALUE_VALUE_NOT_SET,
                              Sprintf("Data must be present at position %" PRIu32, i));

        CHECK_OR_RETURN_ERROR(v.ListSize() == 0 &&
                              v.StructSize() == 0 &&
                              v.TupleSize() == 0 &&
                              v.DictSize() == 0,
                              Sprintf("Simple type is expected in tuple at position %" PRIu32, i));

        TCell c;
        switch (types[i]) {

#define CASE_SIMPLE_TYPE(name, type, protoField) \
        case NScheme::NTypeIds::name: \
        { \
            bool valuePresent = v.Has##protoField(); \
            if (valuePresent) { \
                type val = v.Get##protoField(); \
                c = TCell((const char*)&val, sizeof(val)); \
            } else if (allowCastFromString && v.HasText()) { \
                const auto slot = NUdf::GetDataSlot(types[i]); \
                const auto out = NMiniKQL::ValueFromString(slot, v.GetText()); \
                CHECK_OR_RETURN_ERROR(out, Sprintf("Cannot parse value of type " #name " from text '%s' in tuple at position %" PRIu32, v.GetText().data(), i)); \
                const auto val = out.Get<type>(); \
                c = TCell((const char*)&val, sizeof(val)); \
            } else { \
                CHECK_OR_RETURN_ERROR(false, Sprintf("Value of type " #name " expected in tuple at position %" PRIu32, i)); \
            } \
            Y_VERIFY(c.IsInline()); \
            break; \
        }

        CASE_SIMPLE_TYPE(Bool,   bool,  Bool);
        CASE_SIMPLE_TYPE(Int8,   i8,    Int32);
        CASE_SIMPLE_TYPE(Uint8,  ui8,   Uint32);
        CASE_SIMPLE_TYPE(Int16,  i16,   Int32);
        CASE_SIMPLE_TYPE(Uint16, ui16,  Uint32);
        CASE_SIMPLE_TYPE(Int32,  i32,   Int32);
        CASE_SIMPLE_TYPE(Uint32, ui32,  Uint32);
        CASE_SIMPLE_TYPE(Int64,  i64,   Int64);
        CASE_SIMPLE_TYPE(Uint64, ui64,  Uint64);
        CASE_SIMPLE_TYPE(Float,  float, Float);
        CASE_SIMPLE_TYPE(Double, double,Double);
        CASE_SIMPLE_TYPE(Date,   ui16,  Uint32);
        CASE_SIMPLE_TYPE(Datetime, ui32, Uint32);
        CASE_SIMPLE_TYPE(Timestamp, ui64, Uint64);
        CASE_SIMPLE_TYPE(Interval, i64, Int64);


#undef CASE_SIMPLE_TYPE

        case NScheme::NTypeIds::Yson:
        case NScheme::NTypeIds::Json:
        case NScheme::NTypeIds::Utf8:
        {
            c = TCell(v.GetText().data(), v.GetText().size());
            break;
        }
        case NScheme::NTypeIds::JsonDocument:
        case NScheme::NTypeIds::DyNumber:
        {
            c = TCell(v.GetBytes().data(), v.GetBytes().size());
            break;
        }
        case NScheme::NTypeIds::String:
        {
            if (v.HasBytes()) {
                c = TCell(v.GetBytes().data(), v.GetBytes().size());
            } else if (allowCastFromString && v.HasText()) {
                c = TCell(v.GetText().data(), v.GetText().size());
            } else {
                CHECK_OR_RETURN_ERROR(false, Sprintf("Cannot parse value of type String in tuple at position %" PRIu32, i));
            }
            break;
        }
        default:
            CHECK_OR_RETURN_ERROR(false, Sprintf("Unsupported typeId %" PRIu16 " at index %" PRIu32, types[i], i));
            break;
        }

        CHECK_OR_RETURN_ERROR(!c.IsNull(), Sprintf("Invalid non-NULL value at index %" PRIu32, i));
        key.push_back(c);
    }

#undef CHECK_OR_RETURN_ERROR

    return true;
}

bool CellToValue(NScheme::TTypeId typeId, const TCell& c, NKikimrMiniKQL::TValue& val, TString& errStr) {
    if (c.IsNull()) {
        return true;
    }

    switch (typeId) {
    case NScheme::NTypeIds::Int8:
        Y_VERIFY(c.Size() == sizeof(i8));
        val.MutableOptional()->SetInt32(*(i8*)c.Data());
        break;
    case NScheme::NTypeIds::Uint8:
        Y_VERIFY(c.Size() == sizeof(ui8));
        val.MutableOptional()->SetUint32(*(ui8*)c.Data());
        break;

    case NScheme::NTypeIds::Int16:
        Y_VERIFY(c.Size() == sizeof(i16));
        val.MutableOptional()->SetInt32(ReadUnaligned<i16>(c.Data()));
        break;
    case NScheme::NTypeIds::Uint16:
        Y_VERIFY(c.Size() == sizeof(ui16));
        val.MutableOptional()->SetUint32(ReadUnaligned<ui16>(c.Data()));
        break;

    case NScheme::NTypeIds::Int32:
        Y_VERIFY(c.Size() == sizeof(i32));
        val.MutableOptional()->SetInt32(ReadUnaligned<i32>(c.Data()));
        break;
    case NScheme::NTypeIds::Uint32:
        Y_VERIFY(c.Size() == sizeof(ui32));
        val.MutableOptional()->SetUint32(ReadUnaligned<ui32>(c.Data()));
        break;

    case NScheme::NTypeIds::Int64:
        Y_VERIFY(c.Size() == sizeof(i64));
        val.MutableOptional()->SetInt64(ReadUnaligned<i64>(c.Data()));
        break;
    case NScheme::NTypeIds::Uint64:
        Y_VERIFY(c.Size() == sizeof(ui64));
        val.MutableOptional()->SetUint64(ReadUnaligned<ui64>(c.Data()));
        break;

    case NScheme::NTypeIds::Bool:
        Y_VERIFY(c.Size() == sizeof(bool));
        val.MutableOptional()->SetBool(*(bool*)c.Data());
        break;

    case NScheme::NTypeIds::Float:
        Y_VERIFY(c.Size() == sizeof(float));
        val.MutableOptional()->SetFloat(ReadUnaligned<float>(c.Data()));
        break;

    case NScheme::NTypeIds::Double:
        Y_VERIFY(c.Size() == sizeof(double));
        val.MutableOptional()->SetDouble(ReadUnaligned<double>(c.Data()));
        break;

    case NScheme::NTypeIds::Date:
        Y_VERIFY(c.Size() == sizeof(ui16));
        val.MutableOptional()->SetUint32(ReadUnaligned<i16>(c.Data()));
        break;
    case NScheme::NTypeIds::Datetime:
        Y_VERIFY(c.Size() == sizeof(ui32));
        val.MutableOptional()->SetUint32(ReadUnaligned<ui32>(c.Data()));
        break;
    case NScheme::NTypeIds::Timestamp:
        Y_VERIFY(c.Size() == sizeof(ui64));
        val.MutableOptional()->SetUint64(ReadUnaligned<ui64>(c.Data()));
        break;
    case NScheme::NTypeIds::Interval:
        Y_VERIFY(c.Size() == sizeof(i64));
        val.MutableOptional()->SetInt64(ReadUnaligned<i64>(c.Data()));
        break;

    case NScheme::NTypeIds::JsonDocument:
    case NScheme::NTypeIds::String:
    case NScheme::NTypeIds::DyNumber:
        val.MutableOptional()->SetBytes(c.Data(), c.Size());
        break;

    case NScheme::NTypeIds::Json:
    case NScheme::NTypeIds::Yson:
    case NScheme::NTypeIds::Utf8:
        val.MutableOptional()->SetText(c.Data(), c.Size());
        break;
    default:
        errStr = "Unknown type: " + ToString(typeId);
        return false;
    }
    return true;
}


} // namspace NMiniKQL
} // namspace NKikimr
