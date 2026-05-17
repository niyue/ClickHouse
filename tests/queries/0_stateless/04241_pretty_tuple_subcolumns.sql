SET output_format_pretty_color = 0;
SET output_format_pretty_row_numbers = 1;
SET output_format_pretty_display_footer_column_names = 0;
SET output_format_pretty_squash_consecutive_ms = 0;
SET enable_named_columns_in_function_tuple = 1;

SELECT 'hello' AS x, (1 AS a, 'world' AS b) AS t FORMAT PrettyCompactNoEscapes;
SELECT 'hello' AS x, (1 AS a, 'world' AS b) AS t FORMAT Pretty;
SELECT 'hello' AS x, (1 AS a, 'world' AS b) AS t FORMAT Vertical;
