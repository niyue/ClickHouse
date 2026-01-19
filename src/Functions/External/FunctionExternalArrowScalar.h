#pragma once

#include <memory>
#include <string>

#include <Extensions/ClickHouseExtensionAPI.h>
#include <Functions/IFunction.h>
#include <Interpreters/Context_fwd.h>

namespace DB
{

struct ExternalArrowScalarFunctionData
{
    ExternalArrowScalarFunctionData(
        std::string name_,
        CHArrowScalarFunction function_,
        const CHFunctionMeta & meta_,
        ContextPtr context_);

    std::string name;
    CHArrowScalarFunction function = nullptr;
    DataTypePtr return_type;
    size_t num_args = 0;
    bool is_variadic = false;
    bool deterministic = true;
};

class FunctionExternalArrowScalar final : public IFunction
{
public:
    explicit FunctionExternalArrowScalar(std::shared_ptr<ExternalArrowScalarFunctionData> data_);

    String getName() const override { return data->name; }
    size_t getNumberOfArguments() const override { return data->is_variadic ? 0 : data->num_args; }
    bool isVariadic() const override { return data->is_variadic; }
    bool isDeterministic() const override { return data->deterministic; }
    bool isDeterministicInScopeOfQuery() const override { return data->deterministic; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override;

    ColumnPtr executeImpl(
        const ColumnsWithTypeAndName & arguments,
        const DataTypePtr & result_type,
        size_t input_rows_count) const override;

private:
    std::shared_ptr<ExternalArrowScalarFunctionData> data;
};

}
