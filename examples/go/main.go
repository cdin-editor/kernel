// Package main demonstrates calling the cdin-kernel bridge through cgo.

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../ffi -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -lcdin_ffi -Wl,-rpath,${SRCDIR}/../../build
#include <stdlib.h>
#include "cdin_ffi.h"
*/
import "C"

import (
	"fmt"
	"os"
	"unsafe"
)

func check(status C.cdin_status) error {
	if status == C.CDIN_OK {
		return nil
	}
	return fmt.Errorf("cdin call failed: %s (%d)", C.GoString(C.cdin_ffi_status_name(status)), int(status))
}

func main() {
	var kernel *C.cdin_kernel
	var doc C.cdin_doc
	if err := check(C.cdin_ffi_kernel_create(&kernel)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer func() {
		if kernel != nil {
			if err := check(C.cdin_ffi_kernel_destroy(kernel)); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		}
	}()

	original := []byte("Hello world\n")
	cOriginal := C.CBytes(original)
	status := C.cdin_ffi_doc_create(kernel, (*C.uint8_t)(cOriginal), C.size_t(len(original)), &doc)
	C.free(cOriginal)
	if err := check(status); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer func() {
		if err := check(C.cdin_ffi_doc_close(kernel, doc)); err != nil {
			fmt.Fprintln(os.Stderr, err)
		}
	}()

	replacement := []byte("CDIN")
	cReplacement := C.CBytes(replacement)
	var version C.cdin_version
	status = C.cdin_ffi_doc_replace(kernel, doc, 0, 6, 11,
		(*C.uint8_t)(cReplacement), C.size_t(len(replacement)), &version)
	C.free(cReplacement)
	if err := check(status); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	var required C.size_t
	if err := check(C.cdin_ffi_doc_read(kernel, doc, nil, 0, &required)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	buffer := C.malloc(required)
	if buffer == nil && required != 0 {
		fmt.Fprintln(os.Stderr, "out of memory")
		os.Exit(1)
	}
	defer C.free(buffer)
	var actual C.size_t
	if err := check(C.cdin_ffi_doc_read(kernel, doc, (*C.uint8_t)(buffer), required, &actual)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if actual > 0 {
		fmt.Print(C.GoStringN((*C.char)(unsafe.Pointer(buffer)), C.int(actual)))
	}
	fmt.Fprintf(os.Stderr, "document version: %d\n", uint64(version))
}
