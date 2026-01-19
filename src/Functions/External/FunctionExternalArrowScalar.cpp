#include <Functions/External/FunctionExternalArrowScalar.h>

#include "config.h"

#include <Columns/ColumnConst.h>
#include <Columns/ColumnVector.h>
#include <Core/Block.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/IDataType.h>
#include <Processors/Formats/Impl/ArrowColumnToCHColumn.h>
#include <Processors/Formats/Impl/CHColumnToArrowColumn.h>
#include <Formats/FormatSettings.h>
#include <Common/Exception.h>

#include <optional>

#if USE_ARROW
#include <arrow/api.h>
#include <arrow/c/abi.h>
#include <arrow/c/bridge.h>
#include <arrow/buffer.h>
#include <arrow/table.h>
#endif

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_CONVERT_TYPE;
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int UNSUPPORTED_METHOD;
}

ExternalArrowScalarFunctionData::ExternalArrowScalarFunctionData(
    std::string name_,
    CHArrowScalarFunction function_,
    const CHFunctionMeta & meta_,
    ContextPtr)
    : name(std::move(name_))
    , function(function_)
    , num_args(meta_.num_args)
    , is_variadic(meta_.is_variadic != 0)
    , deterministic(meta_.deterministic != 0)
{
    if (!function)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function {} has null callback", name);
    if (!meta_.return_type)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function {} has null return_type", name);

    return_type = DataTypeFactory::instance().get(meta_.return_type);
}

FunctionExternalArrowScalar::FunctionExternalArrowScalar(std::shared_ptr<ExternalArrowScalarFunctionData> data_)
    : data(std::move(data_))
{
}

DataTypePtr FunctionExternalArrowScalar::getReturnTypeImpl(const DataTypes & arguments) const
{
    if (!data->is_variadic && arguments.size() != data->num_args)
        throw Exception(
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Function {} expects {} arguments, got {}",
            data->name,
            data->num_args,
            arguments.size());

    return data->return_type;
}

#if USE_ARROW
namespace
{

struct ArrowCData
{
    ArrowSchema schema{};
    ArrowArray array{};
};

std::shared_ptr<arrow::DataType> getArrowNumericType(const IDataType & type)
{
    WhichDataType which(type);

    if (which.isInt8())
        return arrow::int8();
    if (which.isInt16())
        return arrow::int16();
    if (which.isInt32())
        return arrow::int32();
    if (which.isInt64())
        return arrow::int64();
    if (which.isUInt8())
        return arrow::uint8();
    if (which.isUInt16())
        return arrow::uint16();
    if (which.isUInt32())
        return arrow::uint32();
    if (which.isUInt64())
        return arrow::uint64();
    if (which.isFloat32())
        return arrow::float32();
    if (which.isFloat64())
        return arrow::float64();

    return nullptr;
}

template <typename T, typename ArrowType>
std::shared_ptr<arrow::Array> makeNumericArrayZeroCopy(const ColumnPtr & column, const std::shared_ptr<arrow::DataType> & arrow_type)
{
    const auto & col = assert_cast<const ColumnVector<T> &>(*column);
    const auto & data = col.getData();
    auto buffer = arrow::Buffer::Wrap(
        reinterpret_cast<const uint8_t *>(data.data()),
        static_cast<int64_t>(data.size() * sizeof(T)));

    auto array_data = arrow::ArrayData::Make(arrow_type, static_cast<int64_t>(col.size()), {nullptr, buffer});
    return arrow::MakeArray(array_data);
}

std::shared_ptr<arrow::Array> tryMakeZeroCopyArray(const ColumnWithTypeAndName & column)
{
    if (!column.column || !column.type)
        return nullptr;

    WhichDataType which(*column.type);
    if (which.isUInt8())
    {
        if (typeid_cast<const ColumnVector<UInt8> *>(column.column.get()))
            return makeNumericArrayZeroCopy<UInt8, arrow::UInt8Type>(column.column, arrow::uint8());
        return nullptr;
    }
    if (which.isInt8())
    {
        if (typeid_cast<const ColumnVector<Int8> *>(column.column.get()))
        return makeNumericArrayZeroCopy<Int8, arrow::Int8Type>(column.column, arrow::int8());
        return nullptr;
    }
    if (which.isInt16())
    {
        if (typeid_cast<const ColumnVector<Int16> *>(column.column.get()))
        return makeNumericArrayZeroCopy<Int16, arrow::Int16Type>(column.column, arrow::int16());
        return nullptr;
    }
    if (which.isInt32())
    {
        if (typeid_cast<const ColumnVector<Int32> *>(column.column.get()))
        return makeNumericArrayZeroCopy<Int32, arrow::Int32Type>(column.column, arrow::int32());
        return nullptr;
    }
    if (which.isInt64())
    {
        if (typeid_cast<const ColumnVector<Int64> *>(column.column.get()))
        return makeNumericArrayZeroCopy<Int64, arrow::Int64Type>(column.column, arrow::int64());
        return nullptr;
    }
    if (which.isUInt16())
    {
        if (typeid_cast<const ColumnVector<UInt16> *>(column.column.get()))
        return makeNumericArrayZeroCopy<UInt16, arrow::UInt16Type>(column.column, arrow::uint16());
        return nullptr;
    }
    if (which.isUInt32())
    {
        if (typeid_cast<const ColumnVector<UInt32> *>(column.column.get()))
        return makeNumericArrayZeroCopy<UInt32, arrow::UInt32Type>(column.column, arrow::uint32());
        return nullptr;
    }
    if (which.isUInt64())
    {
        if (typeid_cast<const ColumnVector<UInt64> *>(column.column.get()))
        return makeNumericArrayZeroCopy<UInt64, arrow::UInt64Type>(column.column, arrow::uint64());
        return nullptr;
    }
    if (which.isFloat32())
    {
        if (typeid_cast<const ColumnVector<Float32> *>(column.column.get()))
        return makeNumericArrayZeroCopy<Float32, arrow::FloatType>(column.column, arrow::float32());
        return nullptr;
    }
    if (which.isFloat64())
    {
        if (typeid_cast<const ColumnVector<Float64> *>(column.column.get()))
        return makeNumericArrayZeroCopy<Float64, arrow::DoubleType>(column.column, arrow::float64());
        return nullptr;
    }

    return nullptr;
}

bool canUseZeroCopyInput(const ColumnsWithTypeAndName & arguments)
{
    for (const auto & arg : arguments)
    {
        if (!arg.type || !arg.column)
            return false;

        WhichDataType which(*arg.type);
        if (which.isNullable())
            return false;

        if (!tryMakeZeroCopyArray(arg))
            return false;
    }
    return true;
}

std::shared_ptr<arrow::RecordBatch> buildZeroCopyRecordBatch(const ColumnsWithTypeAndName & arguments, size_t rows)
{
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    arrays.reserve(arguments.size());
    fields.reserve(arguments.size());

    for (const auto & arg : arguments)
    {
        auto arrow_type = getArrowNumericType(*arg.type);
        if (!arrow_type)
            throw Exception(ErrorCodes::CANNOT_CONVERT_TYPE, "Unsupported type {} for zero-copy Arrow export", arg.type->getName());

        auto array = tryMakeZeroCopyArray(arg);
        if (!array)
            throw Exception(ErrorCodes::CANNOT_CONVERT_TYPE, "Unsupported column for zero-copy Arrow export");

        arrays.emplace_back(std::move(array));
        fields.emplace_back(arrow::field(arg.name, arrow_type, /*nullable*/ false));
    }

    auto schema = arrow::schema(fields);
    return arrow::RecordBatch::Make(schema, static_cast<int64_t>(rows), arrays);
}

std::shared_ptr<arrow::RecordBatch> buildArrowRecordBatchCopy(const ColumnsWithTypeAndName & arguments, size_t rows)
{
    FormatSettings format_settings;
    CHColumnToArrowColumn::Settings arrow_settings;
    arrow_settings.output_string_as_string = true;
    arrow_settings.output_fixed_string_as_fixed_byte_array = format_settings.arrow.output_fixed_string_as_fixed_byte_array;
    arrow_settings.low_cardinality_as_dictionary = format_settings.arrow.low_cardinality_as_dictionary;
    arrow_settings.use_signed_indexes_for_dictionary = format_settings.arrow.use_signed_indexes_for_dictionary;
    arrow_settings.use_64_bit_indexes_for_dictionary = format_settings.arrow.use_64_bit_indexes_for_dictionary;

    auto converter = std::make_unique<CHColumnToArrowColumn>(Block(arguments), "Arrow", arrow_settings);

    Columns columns;
    columns.reserve(arguments.size());
    for (const auto & arg : arguments)
        columns.emplace_back(arg.column);

    Chunk chunk(columns, rows);
    std::vector<Chunk> chunks;
    chunks.emplace_back(std::move(chunk));

    std::shared_ptr<arrow::Table> table;
    converter->chChunkToArrowTable(table, chunks, columns.size());

    auto batch_res = table->CombineChunksToBatch();
    if (!batch_res.ok())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to combine Arrow chunks: {}", batch_res.status().ToString());

    return *batch_res;
}

ArrowCData exportRecordBatchToCData(const std::shared_ptr<arrow::RecordBatch> & batch)
{
    ArrowCData data{};
    auto status = arrow::ExportRecordBatch(*batch, &data.array, &data.schema);
    if (!status.ok())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to export Arrow record batch: {}", status.ToString());
    return data;
}

std::shared_ptr<arrow::RecordBatch> importRecordBatchFromCData(ArrowSchema * schema, ArrowArray * array)
{
    auto batch_res = arrow::ImportRecordBatch(array, schema);
    if (!batch_res.ok())
    {
        if (array->release)
            array->release(array);
        if (schema->release)
            schema->release(schema);
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to import Arrow record batch: {}", batch_res.status().ToString());
    }
    return *batch_res;
}

ColumnPtr convertArrowOutputToColumn(
    const std::shared_ptr<arrow::RecordBatch> & batch,
    const DataTypePtr & result_type,
    size_t expected_rows)
{
    if (batch->num_columns() != 1)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Extension scalar function must return a single column record batch");

    auto table_res = arrow::Table::FromRecordBatches({batch});
    if (!table_res.ok())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to build Arrow table from batch: {}", table_res.status().ToString());

    auto table = *table_res;
    auto field = batch->schema()->field(0);
    Block header;
    header.insert({result_type, field->name()});

    FormatSettings format_settings;
    ArrowColumnToCHColumn converter(
        header,
        "Arrow",
        format_settings,
        std::nullopt,
        std::nullopt,
        false,
        false,
        format_settings.date_time_overflow_behavior,
        true);

    auto chunk = converter.arrowTableToCHChunk(table, batch->num_rows(), batch->schema()->metadata());
    auto columns = chunk.detachColumns();
    if (columns.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to convert Arrow output to ClickHouse column");

    if (expected_rows != columns[0]->size())
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Extension scalar function returned {} rows, expected {}",
            columns[0]->size(),
            expected_rows);

    return columns[0];
}

}
#endif

ColumnPtr FunctionExternalArrowScalar::executeImpl(
    const ColumnsWithTypeAndName & arguments,
    const DataTypePtr &,
    size_t input_rows_count) const
{
    if (!data->is_variadic && arguments.size() != data->num_args)
        throw Exception(
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Function {} expects {} arguments, got {}",
            data->name,
            data->num_args,
            arguments.size());

#if !USE_ARROW
    throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Arrow support is required for external Arrow UDFs");
#else
    ColumnsWithTypeAndName args = arguments;
    for (auto & arg : args)
        arg.column = arg.column->convertToFullColumnIfConst();

    std::shared_ptr<arrow::RecordBatch> input_batch;
    if (canUseZeroCopyInput(args))
        input_batch = buildZeroCopyRecordBatch(args, input_rows_count);
    else
        input_batch = buildArrowRecordBatchCopy(args, input_rows_count);

    auto input_cdata = exportRecordBatchToCData(input_batch);

    ArrowSchema output_schema{};
    ArrowArray output_array{};
    const char * error_message = nullptr;

    int32_t status = data->function(&input_cdata.schema, &input_cdata.array, &output_schema, &output_array, &error_message);

    if (input_cdata.array.release)
        input_cdata.array.release(&input_cdata.array);
    if (input_cdata.schema.release)
        input_cdata.schema.release(&input_cdata.schema);

    if (status != 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Extension {} failed: {}", data->name, error_message ? error_message : "unknown error");

    auto output_batch = importRecordBatchFromCData(&output_schema, &output_array);
    return convertArrowOutputToColumn(output_batch, data->return_type, input_rows_count);
#endif
}

}
