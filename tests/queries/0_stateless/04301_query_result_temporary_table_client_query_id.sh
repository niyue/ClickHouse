#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

QUERY_ID="query_result_client_${CLICKHOUSE_DATABASE}_${RANDOM}_${RANDOM}"

${CLICKHOUSE_CLIENT} --query_id="${QUERY_ID}" --multiquery --query "
    SELECT number AS x
    FROM numbers(4)
    ORDER BY x
    SETTINGS
        create_temporary_table_for_query_result = 1,
        query_result_temporary_table_max_rows = 2,
        query_result_temporary_table_overflow_mode = 'truncate';

    SELECT groupArray(x)
    FROM
    (
        SELECT x
        FROM \`${QUERY_ID}\`
        ORDER BY x
    )
    FORMAT TSV"
