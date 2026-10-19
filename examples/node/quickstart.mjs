import koffi from 'koffi';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const defaultName = process.platform === 'win32'
  ? 'cdin_ffi.dll'
  : process.platform === 'darwin'
    ? 'libcdin_ffi.dylib'
    : 'libcdin_ffi.so';
const libraryPath = process.env.CDIN_FFI_LIB ?? path.join(root, 'build', defaultName);
const lib = koffi.load(libraryPath);

const kernelCreate = lib.func('int cdin_ffi_kernel_create(_Out_ void **out)');
const kernelDestroy = lib.func('int cdin_ffi_kernel_destroy(void *kernel)');
const docCreate = lib.func('int cdin_ffi_doc_create(void *kernel, const uint8_t *bytes, size_t len, _Out_ uint64_t *out_doc)');
const docReplace = lib.func('int cdin_ffi_doc_replace(void *kernel, uint64_t doc, uint64_t expected_version, uint64_t start, uint64_t end, const uint8_t *bytes, size_t len, _Out_ uint64_t *out_new_version)');
const docRead = lib.func('int cdin_ffi_doc_read(void *kernel, uint64_t doc, _Out_ uint8_t *buf, size_t capacity, _Out_ size_t *out_len)');
const docClose = lib.func('int cdin_ffi_doc_close(void *kernel, uint64_t doc)');
const statusName = lib.func('const char *cdin_ffi_status_name(int status)');

function check(status) {
  if (status !== 0) {
    throw new Error(`cdin call failed: ${statusName(status)} (${status})`);
  }
}

let kernelOut = [0n];
let docOut = [0n];
let kernel = 0n;
let doc = 0n;
try {
  check(kernelCreate(kernelOut));
  kernel = kernelOut[0];

  const initial = Buffer.from('Hello world\n', 'utf8');
  check(docCreate(kernel, initial, initial.length, docOut));
  doc = docOut[0];

  const replacement = Buffer.from('CDIN', 'utf8');
  const newVersion = [0n];
  check(docReplace(kernel, doc, 0n, 6n, 11n, replacement, replacement.length, newVersion));

  const required = [0];
  check(docRead(kernel, doc, null, 0, required));
  const output = Buffer.alloc(required[0]);
  const actual = [0];
  check(docRead(kernel, doc, output, output.length, actual));
  process.stdout.write(output.subarray(0, actual[0]));
  console.error(`document version: ${newVersion[0]}`);
} finally {
  if (kernel !== 0n && doc !== 0n) check(docClose(kernel, doc));
  if (kernel !== 0n) check(kernelDestroy(kernel));
}
