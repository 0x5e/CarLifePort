package main

import (
	"context"
	"errors"
	"log"
	"os/exec"
	"time"
)

// adbTimeout bounds each adb call. adb talks to the phone through its own daemon and can
// hang when the phone is in a bad state; starting up must not wait for it.
const adbTimeout = 10 * time.Second

const weChatWarning = `WeChat is running.

WeChat performs an AOA handshake with Android phones on its own and takes exclusive
ownership of the whole USB device. libusb then cannot claim it (LIBUSB_ERROR_ACCESS), and
adb kill-server does not help either.

Quit WeChat, then start carlifed again.
`

// preflight prepares the USB link before the bridge is started. Both steps come from
// scripts/run.sh.
//
// The WeChat check refuses to start: without it the failure shows up much later as a claim
// error that says nothing about its cause.
//
// The USB reset is best effort. Accessory mode sticks across reconnects, so clearing the
// phone's USB functions here makes the bridge run a full 51/52/53 handshake instead of
// attaching to the accessory session left over from a previous run. No phone, no
// authorization, or no adb at all is not an error: the bridge waits for a device anyway.
func preflight() error {
	if weChatRunning() {
		return errors.New(weChatWarning)
	}
	resetPhoneUSB()
	return nil
}

// weChatRunning reports whether the desktop WeChat client is running. There is no portable
// way to ask, and a missing pgrep simply means "not running".
func weChatRunning() bool {
	if _, err := exec.LookPath("pgrep"); err != nil {
		return false
	}
	return exec.Command("pgrep", "-x", "WeChat").Run() == nil
}

func resetPhoneUSB() {
	if _, err := exec.LookPath("adb"); err != nil {
		log.Printf("adb not found, skipping the USB mode reset")
		return
	}
	log.Printf("resetting the phone USB mode")
	if err := runAdb("shell", "svc", "usb", "setFunctions"); err != nil {
		log.Printf("  no phone connected or not authorized, skipping (plug it in any time)")
	} else {
		log.Printf("  cleared accessory mode")
	}
	// Nothing to stop is fine, hence no error check.
	_ = runAdb("kill-server")
}

func runAdb(args ...string) error {
	ctx, cancel := context.WithTimeout(context.Background(), adbTimeout)
	defer cancel()
	return exec.CommandContext(ctx, "adb", args...).Run()
}
