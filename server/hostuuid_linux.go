//go:build linux

package main

import (
	"os"
	"strings"
)

// hostUUID returns the machine id Linux generates for the installation, which is what the
// system itself considers to identify this machine. /etc/machine-id is the path systemd
// and dbus share; the dbus path below it is the older location and is still the only one
// on distributions without systemd. An empty return means it could not be read.
func hostUUID() string {
	for _, path := range []string{"/etc/machine-id", "/var/lib/dbus/machine-id"} {
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		if id := strings.TrimSpace(string(data)); id != "" {
			return id
		}
	}
	return ""
}
