#pragma once

#include <Processors/IProcessor.h>
#include <Core/Block_fwd.h>
#include <atomic>
#include <memory>

namespace DB
{

enum class QueryResultLimitOverflowMode : uint8_t
{
    Drop,
    Truncate,
};

/// Passes chunks to a query result table until row/byte limits are exceeded.
/// Then it closes its input from the result fork so the primary query result can continue.
class QueryResultLimitTransform final : public IProcessor
{
public:
    QueryResultLimitTransform(
        SharedHeader header_,
        UInt64 max_rows_,
        UInt64 max_bytes_,
        QueryResultLimitOverflowMode overflow_mode_,
        std::shared_ptr<std::atomic_bool> exceeded_);

    String getName() const override { return "QueryResultLimit"; }
    Status prepare() override;

    InputPort & getInputPort() { return inputs.front(); }
    OutputPort & getOutputPort() { return outputs.front(); }

private:
    UInt64 max_rows = 0;
    UInt64 max_bytes = 0;
    UInt64 rows = 0;
    UInt64 bytes = 0;
    bool finish_after_output = false;
    QueryResultLimitOverflowMode overflow_mode;
    std::shared_ptr<std::atomic_bool> exceeded;

    bool wouldExceedLimits(const Chunk & chunk) const;
    Chunk truncateChunk(Chunk chunk) const;
};

}
