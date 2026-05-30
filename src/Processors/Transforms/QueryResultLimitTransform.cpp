#include <Processors/Transforms/QueryResultLimitTransform.h>

#include <Columns/IColumn.h>
#include <Processors/Chunk.h>
#include <Processors/Port.h>

namespace DB
{

QueryResultLimitTransform::QueryResultLimitTransform(
    SharedHeader header_,
    UInt64 max_rows_,
    UInt64 max_bytes_,
    QueryResultLimitOverflowMode overflow_mode_,
    std::shared_ptr<std::atomic_bool> exceeded_)
    : IProcessor(InputPorts{header_}, OutputPorts{std::move(header_)})
    , max_rows(max_rows_)
    , max_bytes(max_bytes_)
    , overflow_mode(overflow_mode_)
    , exceeded(std::move(exceeded_))
{
}

bool QueryResultLimitTransform::wouldExceedLimits(const Chunk & chunk) const
{
    if (max_rows && chunk.getNumRows() > max_rows - std::min(rows, max_rows))
        return true;

    if (max_bytes && chunk.bytes() > max_bytes - std::min(bytes, max_bytes))
        return true;

    return false;
}

Chunk QueryResultLimitTransform::truncateChunk(Chunk chunk) const
{
    if (!max_rows || rows >= max_rows)
        return {};

    const auto rows_to_take = std::min<UInt64>(chunk.getNumRows(), max_rows - rows);
    auto columns = chunk.detachColumns();
    for (auto & column : columns)
        column = column->cut(0, rows_to_take);

    Chunk result(std::move(columns), rows_to_take);
    result.setChunkInfos(std::move(chunk.getChunkInfos()));
    return result;
}

IProcessor::Status QueryResultLimitTransform::prepare()
{
    auto & input = inputs.front();
    auto & output = outputs.front();

    if (finish_after_output)
    {
        output.finish();
        return Status::Finished;
    }

    if (output.isFinished())
    {
        input.close();
        return Status::Finished;
    }

    if (input.isFinished())
    {
        output.finish();
        return Status::Finished;
    }

    if (!output.canPush())
    {
        input.setNotNeeded();
        return Status::PortFull;
    }

    input.setNeeded();
    if (!input.hasData())
        return Status::NeedData;

    auto data = input.pullData(true);

    if (data.exception)
    {
        output.pushException(data.exception);
        return Status::PortFull;
    }

    const bool exceeds_limits = wouldExceedLimits(data.chunk);
    if (exceeds_limits)
    {
        input.close();

        if (overflow_mode == QueryResultLimitOverflowMode::Drop)
        {
            exceeded->store(true, std::memory_order_relaxed);
            output.finish();
            return Status::Finished;
        }

        auto truncated_chunk = truncateChunk(std::move(data.chunk));
        if (!truncated_chunk || (max_bytes && truncated_chunk.bytes() > max_bytes - std::min(bytes, max_bytes)))
        {
            output.finish();
            return Status::Finished;
        }

        bytes += truncated_chunk.bytes();
        rows += truncated_chunk.getNumRows();
        output.push(std::move(truncated_chunk));
        finish_after_output = true;
        return Status::PortFull;
    }

    rows += data.chunk.getNumRows();
    bytes += data.chunk.bytes();
    output.pushData(std::move(data));
    return Status::PortFull;
}

}
