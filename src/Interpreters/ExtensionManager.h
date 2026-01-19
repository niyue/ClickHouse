#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Extensions/ClickHouseExtensionAPI.h>
#include <Interpreters/Context_fwd.h>

namespace Poco::Util
{
class AbstractConfiguration;
}

namespace DB
{

class ExtensionManager
{
public:
    explicit ExtensionManager(ContextPtr context_);

    void loadFromConfig(const Poco::Util::AbstractConfiguration & config);
    void registerScalarFunction(
        const std::string & name,
        CHArrowScalarFunction function,
        const CHFunctionMeta & meta);

private:
    struct LoadedExtension
    {
        std::string path;
        void * handle = nullptr;
    };

    std::vector<LoadedExtension> loaded_extensions;
    ContextPtr context;
};

}
