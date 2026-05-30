#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

BASE="query_result_table_${CLICKHOUSE_DATABASE}_${RANDOM}_${RANDOM}"
SESSION_ID="${BASE}_session"
URL="${CLICKHOUSE_URL}&session_id=${SESSION_ID}&session_timeout=60"

function query_with_id()
{
    local query_id="$1"
    local settings="$2"
    local query="$3"

    ${CLICKHOUSE_CURL} -sS "${URL}&query_id=${query_id}${settings}" --data-binary "${query}"
}

QUERY_ID="${BASE}_query"
query_with_id "${QUERY_ID}" "&create_temporary_table_for_query_result=1" \
    "SELECT number + 1 AS x FROM numbers(3) ORDER BY x FORMAT TSV"

echo "stored"
${CLICKHOUSE_CURL} -sS "${URL}" --data-binary \
    "SELECT groupArray(x) FROM (SELECT x FROM \`${QUERY_ID}\` ORDER BY x) FORMAT TSV"

query_with_id "${QUERY_ID}" "&create_temporary_table_for_query_result=1&query_result_temporary_table_engine=Memory" \
    "SELECT number + 10 AS x FROM numbers(2) ORDER BY x FORMAT TSV"

echo "replaced"
${CLICKHOUSE_CURL} -sS "${URL}" --data-binary \
    "SELECT groupArray(x) FROM (SELECT x FROM \`${QUERY_ID}\` ORDER BY x) FORMAT TSV"

OVERFLOW_QUERY_ID="${BASE}_overflow"
query_with_id "${OVERFLOW_QUERY_ID}" "&create_temporary_table_for_query_result=1&query_result_temporary_table_max_rows=2" \
    "SELECT number AS x FROM numbers(3) ORDER BY x FORMAT TSV"

echo "overflow exists"
${CLICKHOUSE_CURL} -sS "${URL}" --data-binary \
    "EXISTS TEMPORARY TABLE \`${OVERFLOW_QUERY_ID}\` FORMAT TSV"

TRUNCATE_QUERY_ID="${BASE}_truncate"
query_with_id "${TRUNCATE_QUERY_ID}" "&create_temporary_table_for_query_result=1&query_result_temporary_table_max_rows=2&query_result_temporary_table_overflow_mode=truncate" \
    "SELECT number AS x FROM numbers(3) ORDER BY x FORMAT TSV"

echo "truncated"
${CLICKHOUSE_CURL} -sS "${URL}" --data-binary \
    "SELECT groupArray(x) FROM (SELECT x FROM \`${TRUNCATE_QUERY_ID}\` ORDER BY x) FORMAT TSV"

MANUAL_QUERY_ID="${BASE}_manual"
${CLICKHOUSE_CURL} -sS "${URL}" --data-binary \
    "CREATE TEMPORARY TABLE \`${MANUAL_QUERY_ID}\` (x UInt64) ENGINE = Memory"

echo "manual conflict"
query_with_id "${MANUAL_QUERY_ID}" "&create_temporary_table_for_query_result=1" \
    "SELECT 42 AS x FORMAT TSV" 2>&1 | grep -c "already exists"

${CLICKHOUSE_CURL} -sS "${URL}&close_session=1" --data-binary "SELECT 1 FORMAT Null"
