use std::ffi::{c_char, c_void};
use std::ptr;

use arrow_array::ffi::{from_ffi, to_ffi, FFI_ArrowArray};
use arrow_array::{Array, RecordBatch, StructArray};
use arrow_schema::ffi::FFI_ArrowSchema;
use arrow_udf::function;

#[repr(C)]
pub struct CHFunctionMeta {
    return_type: *const c_char,
    num_args: u32,
    is_variadic: u8,
    deterministic: u8,
}

pub type CHArrowScalarFunction = unsafe extern "C" fn(
    input_schema: *const FFI_ArrowSchema,
    input_array: *const FFI_ArrowArray,
    output_schema: *mut FFI_ArrowSchema,
    output_array: *mut FFI_ArrowArray,
    error_message: *mut *const c_char,
) -> i32;

pub type CHRegisterScalarFunction = unsafe extern "C" fn(
    user_data: *mut c_void,
    name: *const c_char,
    function: CHArrowScalarFunction,
    meta: *const CHFunctionMeta,
);

#[repr(C)]
pub struct ClickHouseExtensionAPI {
    api_version: u32,
    user_data: *mut c_void,
    register_scalar: Option<CHRegisterScalarFunction>,
    log: Option<unsafe extern "C" fn(level: i32, msg: *const c_char)>,
}

#[function("add_one(int64) -> int64", output = "eval_add_one")]
fn add_one(x: i64) -> i64 {
    x + 1
}

#[function("add_i64(int64, int64) -> int64", output = "eval_add_i64")]
fn add_i64(a: i64, b: i64) -> i64 {
    a + b
}

#[function("mul_i64(int64, int64) -> int64", output = "eval_mul_i64")]
fn mul_i64(a: i64, b: i64) -> i64 {
    a * b
}

#[function("neg_i64(int64) -> int64", output = "eval_neg_i64")]
fn neg_i64(x: i64) -> i64 {
    -x
}

#[function("append_exclaim(string) -> string", output = "eval_append_exclaim")]
fn append_exclaim(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 1);
    out.push_str(s);
    out.push('!');
    out
}

static RETURN_TYPE: &[u8] = b"Int64\0";
static RETURN_TYPE_STRING: &[u8] = b"String\0";
static FUNCTION_NAME: &[u8] = b"add_one\0";
static FUNCTION_NAME_ADD_I64: &[u8] = b"add_i64\0";
static FUNCTION_NAME_MUL_I64: &[u8] = b"mul_i64\0";
static FUNCTION_NAME_NEG_I64: &[u8] = b"neg_i64\0";
static FUNCTION_NAME_APPEND_EXCLAIM: &[u8] = b"append_exclaim\0";

#[no_mangle]
pub unsafe extern "C" fn clickhouse_extension_init(api: *const ClickHouseExtensionAPI) -> i32 {
    if api.is_null() {
        return -1;
    }
    let api = &*api;
    let register = match api.register_scalar {
        Some(f) => f,
        None => return -1,
    };

    let meta = CHFunctionMeta {
        return_type: RETURN_TYPE.as_ptr() as *const c_char,
        num_args: 1,
        is_variadic: 0,
        deterministic: 1,
    };

    register(
        api.user_data,
        FUNCTION_NAME.as_ptr() as *const c_char,
        eval_add_one_c,
        &meta,
    );

    let meta_add = CHFunctionMeta {
        return_type: RETURN_TYPE.as_ptr() as *const c_char,
        num_args: 2,
        is_variadic: 0,
        deterministic: 1,
    };

    register(
        api.user_data,
        FUNCTION_NAME_ADD_I64.as_ptr() as *const c_char,
        eval_add_i64_c,
        &meta_add,
    );

    register(
        api.user_data,
        FUNCTION_NAME_MUL_I64.as_ptr() as *const c_char,
        eval_mul_i64_c,
        &meta_add,
    );

    let meta_neg = CHFunctionMeta {
        return_type: RETURN_TYPE.as_ptr() as *const c_char,
        num_args: 1,
        is_variadic: 0,
        deterministic: 1,
    };

    register(
        api.user_data,
        FUNCTION_NAME_NEG_I64.as_ptr() as *const c_char,
        eval_neg_i64_c,
        &meta_neg,
    );

    let meta_string = CHFunctionMeta {
        return_type: RETURN_TYPE_STRING.as_ptr() as *const c_char,
        num_args: 1,
        is_variadic: 0,
        deterministic: 1,
    };

    register(
        api.user_data,
        FUNCTION_NAME_APPEND_EXCLAIM.as_ptr() as *const c_char,
        eval_append_exclaim_c,
        &meta_string,
    );

    0
}

unsafe extern "C" fn eval_add_one_c(
    input_schema: *const FFI_ArrowSchema,
    input_array: *const FFI_ArrowArray,
    output_schema: *mut FFI_ArrowSchema,
    output_array: *mut FFI_ArrowArray,
    error_message: *mut *const c_char,
) -> i32 {
    if input_schema.is_null() || input_array.is_null() || output_schema.is_null() || output_array.is_null() {
        if !error_message.is_null() {
            *error_message = cstr("null pointer input");
        }
        return -1;
    }

    let in_schema = FFI_ArrowSchema::from_raw(input_schema as *mut FFI_ArrowSchema);
    let in_array = FFI_ArrowArray::from_raw(input_array as *mut FFI_ArrowArray);
    let array_data = match from_ffi(in_array, &in_schema) {
        Ok(data) => data,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let struct_array = StructArray::from(array_data);
    let input_batch = RecordBatch::from(struct_array);

    let output_batch = match eval_add_one(&input_batch) {
        Ok(batch) => batch,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let out_struct = StructArray::from(output_batch);
    let out_data = out_struct.into_data();
    let (ffi_array, ffi_schema) = match to_ffi(&out_data) {
        Ok(v) => v,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    ptr::write(output_array, ffi_array);
    ptr::write(output_schema, ffi_schema);
    0
}

unsafe extern "C" fn eval_add_i64_c(
    input_schema: *const FFI_ArrowSchema,
    input_array: *const FFI_ArrowArray,
    output_schema: *mut FFI_ArrowSchema,
    output_array: *mut FFI_ArrowArray,
    error_message: *mut *const c_char,
) -> i32 {
    if input_schema.is_null() || input_array.is_null() || output_schema.is_null() || output_array.is_null() {
        if !error_message.is_null() {
            *error_message = cstr("null pointer input");
        }
        return -1;
    }

    let in_schema = FFI_ArrowSchema::from_raw(input_schema as *mut FFI_ArrowSchema);
    let in_array = FFI_ArrowArray::from_raw(input_array as *mut FFI_ArrowArray);
    let array_data = match from_ffi(in_array, &in_schema) {
        Ok(data) => data,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let struct_array = StructArray::from(array_data);
    let input_batch = RecordBatch::from(struct_array);

    let output_batch = match eval_add_i64(&input_batch) {
        Ok(batch) => batch,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let out_struct = StructArray::from(output_batch);
    let out_data = out_struct.into_data();
    let (ffi_array, ffi_schema) = match to_ffi(&out_data) {
        Ok(v) => v,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    ptr::write(output_array, ffi_array);
    ptr::write(output_schema, ffi_schema);
    0
}

unsafe extern "C" fn eval_mul_i64_c(
    input_schema: *const FFI_ArrowSchema,
    input_array: *const FFI_ArrowArray,
    output_schema: *mut FFI_ArrowSchema,
    output_array: *mut FFI_ArrowArray,
    error_message: *mut *const c_char,
) -> i32 {
    if input_schema.is_null() || input_array.is_null() || output_schema.is_null() || output_array.is_null() {
        if !error_message.is_null() {
            *error_message = cstr("null pointer input");
        }
        return -1;
    }

    let in_schema = FFI_ArrowSchema::from_raw(input_schema as *mut FFI_ArrowSchema);
    let in_array = FFI_ArrowArray::from_raw(input_array as *mut FFI_ArrowArray);
    let array_data = match from_ffi(in_array, &in_schema) {
        Ok(data) => data,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let struct_array = StructArray::from(array_data);
    let input_batch = RecordBatch::from(struct_array);

    let output_batch = match eval_mul_i64(&input_batch) {
        Ok(batch) => batch,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let out_struct = StructArray::from(output_batch);
    let out_data = out_struct.into_data();
    let (ffi_array, ffi_schema) = match to_ffi(&out_data) {
        Ok(v) => v,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    ptr::write(output_array, ffi_array);
    ptr::write(output_schema, ffi_schema);
    0
}

unsafe extern "C" fn eval_neg_i64_c(
    input_schema: *const FFI_ArrowSchema,
    input_array: *const FFI_ArrowArray,
    output_schema: *mut FFI_ArrowSchema,
    output_array: *mut FFI_ArrowArray,
    error_message: *mut *const c_char,
) -> i32 {
    if input_schema.is_null() || input_array.is_null() || output_schema.is_null() || output_array.is_null() {
        if !error_message.is_null() {
            *error_message = cstr("null pointer input");
        }
        return -1;
    }

    let in_schema = FFI_ArrowSchema::from_raw(input_schema as *mut FFI_ArrowSchema);
    let in_array = FFI_ArrowArray::from_raw(input_array as *mut FFI_ArrowArray);
    let array_data = match from_ffi(in_array, &in_schema) {
        Ok(data) => data,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let struct_array = StructArray::from(array_data);
    let input_batch = RecordBatch::from(struct_array);

    let output_batch = match eval_neg_i64(&input_batch) {
        Ok(batch) => batch,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let out_struct = StructArray::from(output_batch);
    let out_data = out_struct.into_data();
    let (ffi_array, ffi_schema) = match to_ffi(&out_data) {
        Ok(v) => v,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    ptr::write(output_array, ffi_array);
    ptr::write(output_schema, ffi_schema);
    0
}

unsafe extern "C" fn eval_append_exclaim_c(
    input_schema: *const FFI_ArrowSchema,
    input_array: *const FFI_ArrowArray,
    output_schema: *mut FFI_ArrowSchema,
    output_array: *mut FFI_ArrowArray,
    error_message: *mut *const c_char,
) -> i32 {
    if input_schema.is_null() || input_array.is_null() || output_schema.is_null() || output_array.is_null() {
        if !error_message.is_null() {
            *error_message = cstr("null pointer input");
        }
        return -1;
    }

    let in_schema = FFI_ArrowSchema::from_raw(input_schema as *mut FFI_ArrowSchema);
    let in_array = FFI_ArrowArray::from_raw(input_array as *mut FFI_ArrowArray);
    let array_data = match from_ffi(in_array, &in_schema) {
        Ok(data) => data,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let struct_array = StructArray::from(array_data);
    let input_batch = RecordBatch::from(struct_array);

    let output_batch = match eval_append_exclaim(&input_batch) {
        Ok(batch) => batch,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    let out_struct = StructArray::from(output_batch);
    let out_data = out_struct.into_data();
    let (ffi_array, ffi_schema) = match to_ffi(&out_data) {
        Ok(v) => v,
        Err(err) => {
            if !error_message.is_null() {
                *error_message = cstr(err.to_string().as_str());
            }
            return -1;
        }
    };

    ptr::write(output_array, ffi_array);
    ptr::write(output_schema, ffi_schema);
    0
}

fn cstr(s: &str) -> *const c_char {
    let cstr = std::ffi::CString::new(s).unwrap_or_else(|_| std::ffi::CString::new("error").unwrap());
    cstr.into_raw()
}
