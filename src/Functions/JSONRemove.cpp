#include <Columns/ColumnConst.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeObject.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/Serializations/SerializationObject.h>
#include <Formats/FormatFactory.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Functions/JSONPath/ASTs/ASTJSONPath.h>
#include <Functions/JSONPath/ASTs/ASTJSONPathMemberAccess.h>
#include <Functions/JSONPath/ASTs/ASTJSONPathRange.h>
#include <Functions/JSONPath/ASTs/ASTJSONPathStar.h>
#include <Functions/JSONPath/Parsers/ParserJSONPath.h>
#include <IO/WriteBufferFromString.h>
#include <Interpreters/Context.h>
#include <Parsers/Lexer.h>

#include "config.h"

#include <optional>

#if USE_RAPIDJSON

/// Prevent stack overflow.
#    define RAPIDJSON_PARSE_DEFAULT_FLAGS (kParseIterativeFlag)

#    include <rapidjson/document.h>
#    include <rapidjson/error/en.h>
#    include <rapidjson/stringbuffer.h>
#    include <rapidjson/writer.h>

namespace DB
{
namespace Setting
{
extern const SettingsUInt64 max_parser_backtracks;
extern const SettingsUInt64 max_parser_depth;
}

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int ILLEGAL_COLUMN;
extern const int INCORRECT_DATA;
}

namespace
{

struct JSONRemovePathStep
{
    enum class Kind : uint8_t
    {
        Member,
        ArrayIndex,
    };

    Kind kind;
    String member_name;
    size_t array_index = 0;
};

using JSONRemovePath = std::vector<JSONRemovePathStep>;

bool isToToken(const Token & token)
{
    return token.type == TokenType::BareWord && token.size() == 2 && (token.begin[0] == 't' || token.begin[0] == 'T')
        && (token.begin[1] == 'o' || token.begin[1] == 'O');
}

bool containsRangeExpression(std::string_view query)
{
    Tokens tokens(query.data(), query.data() + query.size());
    size_t bracket_depth = 0;
    bool previous_token_was_number = false;

    for (size_t i = 0;; ++i)
    {
        const auto & token = tokens[i];
        if (token.type == TokenType::EndOfStream)
            return false;

        if (token.type == TokenType::OpeningSquareBracket)
        {
            ++bracket_depth;
            previous_token_was_number = false;
            continue;
        }

        if (token.type == TokenType::ClosingSquareBracket)
        {
            if (bracket_depth > 0)
                --bracket_depth;
            previous_token_was_number = false;
            continue;
        }

        if (bracket_depth == 0)
            continue;

        if (token.type == TokenType::Comma || (previous_token_was_number && isToToken(token)))
            return true;

        previous_token_was_number = token.type == TokenType::Number;
    }
}

JSONRemovePath parseJSONRemovePath(std::string_view query, uint32_t parse_depth, uint32_t parse_backtracks)
{
    if (containsRangeExpression(query))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function JSON_REMOVE does not allow ranges or wildcards in JSON paths");

    Tokens tokens(query.data(), query.data() + query.size());
    IParser::Pos token_iterator(tokens, parse_depth, parse_backtracks);

    Expected expected;
    ASTPtr ast;
    ParserJSONPath parser;
    if (!parser.parse(token_iterator, ast, expected))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unable to parse JSON path for function JSON_REMOVE");

    const auto * json_path = ast->as<ASTJSONPath>();
    if (!json_path || !json_path->jsonpath_query)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unable to parse JSON path for function JSON_REMOVE");

    const auto & children = json_path->jsonpath_query->children;
    if (children.size() < 2)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function JSON_REMOVE cannot remove the root JSON document");

    JSONRemovePath path;
    path.reserve(children.size() - 1);

    for (size_t i = 1; i < children.size(); ++i)
    {
        if (const auto * member_access = children[i]->as<ASTJSONPathMemberAccess>())
        {
            path.push_back({JSONRemovePathStep::Kind::Member, member_access->member_name, 0});
        }
        else if (const auto * range = children[i]->as<ASTJSONPathRange>())
        {
            if (range->is_star || range->ranges.size() != 1 || range->ranges[0].second != range->ranges[0].first + 1)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function JSON_REMOVE does not allow ranges or wildcards in JSON paths");

            path.push_back({JSONRemovePathStep::Kind::ArrayIndex, {}, range->ranges[0].first});
        }
        else if (children[i]->as<ASTJSONPathStar>())
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function JSON_REMOVE does not allow ranges or wildcards in JSON paths");
        }
        else
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unsupported JSON path element for function JSON_REMOVE");
        }
    }

    return path;
}

bool tryMoveToPathStep(rapidjson::Value *& current, const JSONRemovePathStep & step)
{
    if (step.kind == JSONRemovePathStep::Kind::Member)
    {
        if (!current->IsObject())
            return false;

        std::string member_name{step.member_name};
        auto member = current->FindMember(rapidjson::StringRef(member_name.c_str(), member_name.size()));
        if (member == current->MemberEnd())
            return false;

        current = &member->value;
        return true;
    }

    if (!current->IsArray() || step.array_index >= current->Size())
        return false;

    current = &(*current)[static_cast<rapidjson::SizeType>(step.array_index)];
    return true;
}

void removePath(rapidjson::Value & document, const JSONRemovePath & path)
{
    rapidjson::Value * current = &document;
    for (size_t i = 0; i + 1 < path.size(); ++i)
    {
        if (!tryMoveToPathStep(current, path[i]))
            return;
    }

    const auto & last_step = path.back();
    if (last_step.kind == JSONRemovePathStep::Kind::Member)
    {
        if (!current->IsObject())
            return;

        std::string member_name{last_step.member_name};
        auto member = current->FindMember(rapidjson::StringRef(member_name.c_str(), member_name.size()));
        if (member != current->MemberEnd())
            current->RemoveMember(member);

        return;
    }

    if (current->IsArray() && last_step.array_index < current->Size())
    {
        auto index = static_cast<rapidjson::SizeType>(last_step.array_index);
        current->Erase(current->Begin() + index);
    }
}

std::optional<String> removePathsFromJSON(std::string_view json, const std::vector<JSONRemovePath> & paths)
{
    rapidjson::Document document;
    document.Parse(json.data(), json.size());
    if (document.HasParseError())
        return std::nullopt;

    for (const auto & path : paths)
        removePath(document, path);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);

    return String(buffer.GetString(), buffer.GetSize());
}

class FunctionJSONRemove : public IFunction
{
public:
    static constexpr auto name = "JSONRemove";
    static FunctionPtr create(ContextPtr context) { return std::make_shared<FunctionJSONRemove>(context); }

    explicit FunctionJSONRemove(ContextPtr context)
        : max_parser_depth(context->getSettingsRef()[Setting::max_parser_depth])
        , max_parser_backtracks(context->getSettingsRef()[Setting::max_parser_backtracks])
        , format_settings(getFormatSettings(context))
    {
    }

    String getName() const override { return name; }
    bool isVariadic() const override { return true; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return true; }

    size_t getNumberOfArguments() const override { return 0; }
    bool useDefaultImplementationForConstants() const override { return true; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        FunctionArgumentDescriptor json_arg{
            "json", [](const IDataType & type) { return isString(type) || isObject(type); }, nullptr, "String or JSON"};
        FunctionArgumentDescriptor path_arg{"path", isString, nullptr, "String"};
        validateFunctionArgumentsWithVariadics(*this, arguments, {json_arg, path_arg}, path_arg);

        if (isObject(arguments[0].type))
            return arguments[0].type;

        return std::make_shared<DataTypeNullable>(std::make_shared<DataTypeString>());
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        chassert(arguments.size() >= 2);

        const bool is_string_input = isString(arguments[0].type);
        const bool is_object_input = isObject(arguments[0].type);

        const ColumnString * json_column = nullptr;
        if (is_string_input)
        {
            if (isColumnConst(*arguments[0].column))
                json_column = checkAndGetColumnConstData<ColumnString>(arguments[0].column.get());
            else
                json_column = checkAndGetColumn<ColumnString>(arguments[0].column.get());

            if (!json_column)
                throw Exception(ErrorCodes::ILLEGAL_COLUMN, "First argument of function {} must be String", getName());
        }
        else if (!is_object_input)
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "First argument of function {} must be String or JSON", getName());
        }

        std::vector<JSONRemovePath> paths;
        paths.reserve(arguments.size() - 1);

        const auto parse_depth = static_cast<uint32_t>(max_parser_depth);
        const auto parse_backtracks = static_cast<uint32_t>(max_parser_backtracks);

        for (size_t i = 1; i < arguments.size(); ++i)
        {
            const auto * path_column = checkAndGetColumnConstData<ColumnString>(arguments[i].column.get());
            if (!path_column)
                throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Path arguments of function {} must be constant strings", getName());

            paths.push_back(parseJSONRemovePath(path_column->getDataAt(0), parse_depth, parse_backtracks));
        }

        const bool is_json_const = is_string_input && isColumnConst(*arguments[0].column);
        auto input_object_serialization = is_object_input ? arguments[0].type->getDefaultSerialization() : nullptr;

        if (is_string_input)
        {
            auto null_map = ColumnUInt8::create();
            auto data = ColumnString::create();
            null_map->reserve(input_rows_count);
            data->reserve(input_rows_count);

            for (size_t row = 0; row < input_rows_count; ++row)
            {
                auto string_ref = json_column->getDataAt(is_json_const ? 0 : row);
                auto result = removePathsFromJSON({string_ref.data(), string_ref.size()}, paths);
                if (!result)
                {
                    null_map->insertValue(1);
                    data->insertDefault();
                    continue;
                }

                null_map->insertValue(0);
                data->insertData(result->data(), result->size());
            }

            return ColumnNullable::create(std::move(data), std::move(null_map));
        }

        auto result_column = result_type->createColumn();
        const auto * result_object_serialization = typeid_cast<const SerializationObject *>(result_type->getDefaultSerialization().get());
        if (!result_object_serialization)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Result type of function {} must be JSON", getName());

        for (size_t row = 0; row < input_rows_count; ++row)
        {
            String object_json;
            WriteBufferFromOwnString buffer;
            input_object_serialization->serializeTextJSON(*arguments[0].column, row, buffer, format_settings);
            object_json = buffer.str();

            auto result = removePathsFromJSON(object_json, paths);
            if (!result)
                throw Exception(ErrorCodes::INCORRECT_DATA, "Unable to parse JSON produced from JSON argument of function {}", getName());

            result_object_serialization->deserializeObject(*result_column, *result, format_settings);
        }

        return result_column;
    }

private:
    const size_t max_parser_depth;
    const size_t max_parser_backtracks;
    const FormatSettings format_settings;
};

}

REGISTER_FUNCTION(JSONRemove)
{
    FunctionDocumentation::Description description = R"(
Removes data from a JSON document at one or more JSON paths.
If an element does not exist in the document, no changes are made.
Paths are evaluated from left to right, and the result from each path is used as the input for the next path.
For `String` input, the function returns `NULL` if the JSON document is invalid.
    )";
    FunctionDocumentation::Syntax syntax = "JSON_REMOVE(json, path[, path] ...)";
    FunctionDocumentation::Arguments arguments
        = {{"json", "A string with valid JSON or a JSON value.", {"String", "JSON"}},
           {"path[, path] ...", "One or more constant strings representing JSON paths without ranges or wildcards.", {"String"}}};
    FunctionDocumentation::ReturnedValue returned_value
        = {"Returns the JSON document after removing the specified paths. If the input is `String`, returns `Nullable(String)` and returns "
           "`NULL` for invalid JSON. If the input is `JSON`, returns `JSON`.",
           {"Nullable(String)", "JSON"}};
    FunctionDocumentation::Examples examples
        = {{"Usage example",
            R"(
SELECT JSON_REMOVE('{"a":1,"b":2,"c":3}', '$.b', '$.c');
        )",
            R"(
{"a":1}
        )"}};
    FunctionDocumentation::IntroducedIn introduced_in = {26, 6};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::JSON;
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionJSONRemove>(documentation);
    factory.registerAlias("jsonRemove", "JSONRemove");
    factory.registerAlias("JSON_REMOVE", "JSONRemove", FunctionFactory::Case::Insensitive);
}

}

#endif
