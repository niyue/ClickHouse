#!/usr/bin/expect -f

set script_path [info script]
set CUR_DIR [file dirname [file normalize $script_path]]

set CLICKHOUSE_LOCAL [exec bash -c "source $CUR_DIR/../shell_config.sh && echo \$CLICKHOUSE_LOCAL"]

log_user 0
set timeout 30
match_max 100000

expect_after {
    -i $any_spawn_id timeout { exit 1 }
}

puts "clickhouse-local"

spawn bash -c "$CLICKHOUSE_LOCAL --output-format-pretty-color 0 --highlight 0 --progress no --enable-progress-table-toggle 0"

expect ":) "
send -- "SELECT number AS x FROM numbers(5) ORDER BY x SETTINGS create_temporary_table_for_query_result = 1, query_result_temporary_table_max_rows = 3, query_result_temporary_table_overflow_mode = 'truncate';\r"
expect -re {Query id: ([0-9a-f-]+)}
set query_id $expect_out(1,string)
expect ":) "

send -- "SELECT groupArray(x) FROM (SELECT x FROM `$query_id` ORDER BY x) FORMAT TSV;\r"
expect {
    -exact {[0,1,2]} { puts {[0,1,2]} }
    "UNKNOWN_TABLE" { exit 1 }
}
expect ":) "

send -- "exit\r"
expect eof
