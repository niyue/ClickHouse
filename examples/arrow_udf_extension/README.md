Arrow UDF extension example (Rust)

This is a minimal example for ClickHouse external Arrow scalar UDFs using
Arrow C Data Interface and `arrow-udf`.

Build:
  cargo build --release

The resulting shared library is in:
  target/release/libclickhouse_arrow_udf_example.{so,dylib}

Example ClickHouse config:
  <extensions>
      <library>/path/to/libclickhouse_arrow_udf_example.so</library>
  </extensions>

Notes:
- The example uses Arrow C Data Interface (no IPC).
- Error messages are allocated in Rust and leaked; a production extension
  should provide a free function or reuse a static buffer.
