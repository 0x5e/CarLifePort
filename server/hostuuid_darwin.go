//go:build darwin

package main

import (
	"encoding/hex"
	"syscall"
	"unsafe"
)

// hostUUID returns the hardware UUID macOS gives this machine: the same value
// `ioreg -rd1 -c IOPlatformExpertDevice` prints as IOPlatformUUID, and the one that stays
// the same across reinstalls of this service. It is read through gethostuuid rather than
// by running ioreg, so no child process is needed.
//
// gethostuuid takes a timeout for the lock it takes while the UUID is being generated; the
// stored UUID is returned without waiting, and the timeout only bounds the case where it
// is not there yet. An empty return means the call failed.
func hostUUID() string {
	var id [16]byte
	wait := syscall.Timespec{Sec: 1}
	if _, _, errno := syscall.Syscall(syscall.SYS_GETHOSTUUID,
		uintptr(unsafe.Pointer(&id[0])), uintptr(unsafe.Pointer(&wait)), 0); errno != 0 {
		return ""
	}
	return formatUUID(id[:])
}

// formatUUID renders the 16 bytes the way a UUID is normally written
// (8-4-4-4-12 hex digits). The value goes into a protobuf string, so it is not parsed back
// by anything here; this is the form the phone uploads in its statistics.
func formatUUID(id []byte) string {
	digits := hex.EncodeToString(id)
	return digits[0:8] + "-" + digits[8:12] + "-" + digits[12:16] + "-" + digits[16:20] + "-" + digits[20:32]
}
