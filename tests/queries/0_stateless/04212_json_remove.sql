-- { echoOn }
SELECT JSON_REMOVE('{"a":1,"b":2,"c":3}', '$.b', '$.c');
SELECT JSON_REMOVE('{"A":1,"B":2,"C":{"D":3}}', '$.C');
SELECT JSON_REMOVE('["A","B",["C","D"],"E"]', '$[1]');
SELECT JSON_REMOVE('{"a":{"b":[10,20,30],"c":1}}', '$.a.b[1]');
SELECT JSON_REMOVE('{"a":1}', '$.missing');
SELECT JSON_REMOVE('{"a":[0,1,2,3]}', '$.a[0]', '$.a[1]');
SELECT JSON_REMOVE('{"a b":1,"x":2}', '$["a b"]');
SELECT JSONRemove('{"a":1,"b":2}', '$.a');
SELECT jsonRemove('{"a":1,"b":2}', '$.b');
SELECT JSON_REMOVE('{"a":{"b":1},"c":2}', '$.a.b');
SELECT JSON_REMOVE(materialize('{"a":1,"b":2}')::JSON(max_dynamic_paths=2), '$.a');
SELECT JSON_REMOVE(materialize('{"a":{"b":1},"c":2}')::JSON(max_dynamic_paths=2), '$.a.b');
SELECT toTypeName(JSON_REMOVE(materialize('{"a":1,"b":2}')::JSON(max_dynamic_paths=2), '$.a'));
SELECT JSON_REMOVE(NULL, '$.a');
SELECT JSON_REMOVE(toNullable('{"a":1,"b":2}'), '$.a');
SELECT JSON_REMOVE('{"a":1}', CAST(NULL, 'Nullable(String)'));
SELECT JSON_REMOVE('bad json', '$.a');
SELECT toTypeName(JSON_REMOVE('bad json', '$.a'));

SELECT JSON_REMOVE(); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT JSON_REMOVE('{"a":1}'); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT JSON_REMOVE(1, '$.a'); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT JSON_REMOVE('{"a":1}', 1); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT JSON_REMOVE(materialize('{"a":1}')::JSON(max_dynamic_paths=1), materialize('$.a')); -- { serverError ILLEGAL_COLUMN }
SELECT JSON_REMOVE('{"a":1}', '$'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":1}', '$a'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":1}', '$..a'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":[1,2]}', '$.a[0,1]'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":[1,2]}', '$.a[*]'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":1}', '$.*'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":[1,2]}', '$.a[0 to 1]'); -- { serverError BAD_ARGUMENTS }
SELECT JSON_REMOVE('{"a":1}', materialize('$.a')); -- { serverError ILLEGAL_COLUMN }
