#include <Interpreters/ExtensionManager.h>

#include "config.h"

#include <Extensions/ClickHouseExtensionAPI.h>
#include <Functions/FunctionFactory.h>
#include <Functions/IFunctionAdaptors.h>
#include <Functions/External/FunctionExternalArrowScalar.h>
#include <Common/Exception.h>
#include <Common/getMultipleKeysFromConfig.h>
#include <Interpreters/Context.h>

#include <Poco/Util/AbstractConfiguration.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int UNSUPPORTED_METHOD;
}

namespace
{

void registerScalarFunction(
    void * user_data,
    const char * name,
    CHArrowScalarFunction function,
    const CHFunctionMeta * meta)
{
    if (!user_data)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Extension register_scalar called with null user_data");
    if (!name || !meta || !meta->return_type)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Extension register_scalar requires name and return_type");

    auto * manager = static_cast<ExtensionManager *>(user_data);
    manager->registerScalarFunction(name, function, *meta);
}

}

ExtensionManager::ExtensionManager(ContextPtr context_) : context(std::move(context_))
{
}

void ExtensionManager::loadFromConfig(const Poco::Util::AbstractConfiguration & config)
{
    auto libraries = getMultipleValuesFromConfig(config, "extensions", "library");
    if (libraries.empty())
        return;

#if defined(_WIN32)
    throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Extension loading is not supported on Windows");
#else
    ClickHouseExtensionAPI api{};
    api.api_version = 1;
    api.user_data = this;
    api.register_scalar = &DB::registerScalarFunction;
    api.log = nullptr;

    for (const auto & path : libraries)
    {
        LoadedExtension extension;
        extension.path = path;

        extension.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!extension.handle)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Failed to load extension {}: {}", path, dlerror());

        auto * init = reinterpret_cast<ClickHouseExtensionInitFn>(dlsym(extension.handle, "clickhouse_extension_init"));
        if (!init)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Extension {} is missing clickhouse_extension_init", path);

        if (init(&api) != 0)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Extension {} failed to initialize", path);

        loaded_extensions.emplace_back(std::move(extension));
    }
#endif
}

void ExtensionManager::registerScalarFunction(
    const std::string & name,
    CHArrowScalarFunction function,
    const CHFunctionMeta & meta)
{
    auto data = std::make_shared<ExternalArrowScalarFunctionData>(name, function, meta, context);

    if (FunctionFactory::instance().has(name))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function {} is already registered", name);

    FunctionFactory::instance().registerFunction(
        name,
        [data](ContextPtr) -> FunctionOverloadResolverPtr
        {
            auto function_impl = std::make_shared<FunctionExternalArrowScalar>(data);
            return std::make_shared<FunctionToOverloadResolverAdaptor>(function_impl);
        });
}

}
