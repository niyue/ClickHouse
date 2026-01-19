#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct ArrowArray;
struct ArrowSchema;

typedef struct CHFunctionMeta
{
    const char * return_type;
    uint32_t num_args;
    uint8_t is_variadic;
    uint8_t deterministic;
} CHFunctionMeta;

typedef int32_t (*CHArrowScalarFunction)(
    const struct ArrowSchema * input_schema,
    const struct ArrowArray * input_array,
    struct ArrowSchema * output_schema,
    struct ArrowArray * output_array,
    const char ** error_message);

typedef void (*CHRegisterScalarFunction)(
    void * user_data,
    const char * name,
    CHArrowScalarFunction function,
    const CHFunctionMeta * meta);

typedef struct ClickHouseExtensionAPI
{
    uint32_t api_version;
    void * user_data;
    CHRegisterScalarFunction register_scalar;
    void (*log)(int level, const char * msg);
} ClickHouseExtensionAPI;

typedef int32_t (*ClickHouseExtensionInitFn)(const ClickHouseExtensionAPI * api);

#ifdef __cplusplus
}
#endif
