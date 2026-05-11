#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-msan

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --enable_analyzer=0 << 'EOF'
DROP TABLE IF EXISTS wasm_mv_src;
DROP TABLE IF EXISTS wasm_mv_dst;
DROP TABLE IF EXISTS wasm_mv;
DROP FUNCTION IF EXISTS wasm_mv_identity;
DELETE FROM system.webassembly_modules WHERE name = 'identity_mv_test';
EOF

cat "${CUR_DIR}/wasm/identity_int.wasm" | ${CLICKHOUSE_CLIENT} --enable_analyzer=0 \
    --query "INSERT INTO system.webassembly_modules (name, code) SELECT 'identity_mv_test', code FROM input('code String') FORMAT RawBlob"

${CLICKHOUSE_CLIENT} --enable_analyzer=0 << 'EOF'
CREATE FUNCTION wasm_mv_identity
    LANGUAGE WASM ABI ROW_DIRECT
    FROM 'identity_mv_test' :: 'identity_raw'
    ARGUMENTS (x Int32) RETURNS Int32;

CREATE TABLE wasm_mv_src (x Int32) ENGINE = Memory;
CREATE TABLE wasm_mv_dst (y Int32) ENGINE = Memory;
CREATE MATERIALIZED VIEW wasm_mv TO wasm_mv_dst AS SELECT wasm_mv_identity(x) AS y FROM wasm_mv_src;

INSERT INTO wasm_mv_src SELECT number::Int32 FROM numbers(5);
SELECT y FROM wasm_mv_dst ORDER BY y;

DROP TABLE wasm_mv;
DROP TABLE wasm_mv_src;
DROP TABLE wasm_mv_dst;
DROP FUNCTION wasm_mv_identity;
DELETE FROM system.webassembly_modules WHERE name = 'identity_mv_test';
EOF
