package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net"
	"sync"
	"time"
)

// Kept in sync with host/protocol.h.
const (
	chCmd   byte = 1
	chVideo byte = 2
	chAudio byte = 3
	chTTS   byte = 4
	chVR    byte = 5
	chCtrl  byte = 6
)

// Upstream events (carlife-host -> this process)
const (
	evStatus               byte = 1
	evProtocolMatch        byte = 2
	evMDInfo               byte = 3
	evVideoInitDone        byte = 4
	evVideoFPSDone         byte = 5
	evForeground           byte = 6
	evBackground           byte = 7
	evScreenOn             byte = 8
	evScreenOff            byte = 9
	evTeleState            byte = 10
	evMicWakeup            byte = 11
	evMicEnd               byte = 12
	evMicRecog             byte = 13
	evMDExit               byte = 14
	evModuleStatus         byte = 15
	evFeatureConfigRequest byte = 16
	evAuthenResult         byte = 17
	evLog                  byte = 90

	evVideoData      byte = 1
	evVideoHeartbeat byte = 2

	evAudioInit   byte = 1
	evAudioData   byte = 2
	evAudioStop   byte = 3
	evAudioPause  byte = 4
	evAudioResume byte = 5
	evAudioSeek   byte = 6
)

// Downstream commands (this process -> carlife-host)
const (
	cmdProtocolVersion   byte = 1
	cmdVideoEncoderInit  byte = 2
	cmdVideoEncoderStart byte = 3
	cmdVideoEncoderPause byte = 4
	cmdVideoEncoderReset byte = 5
	cmdGoToForeground    byte = 6
	cmdLaunchModeNormal  byte = 7
	cmdLaunchModePhone   byte = 8
	cmdLaunchModeMap     byte = 9
	cmdLaunchModeMusic   byte = 10
	cmdAuthenRequest     byte = 11

	cmdTouchAction      byte = 1
	cmdTouchDown        byte = 2
	cmdTouchUp          byte = 3
	cmdTouchMove        byte = 4
	cmdTouchSingleClick byte = 5
	cmdTouchDoubleClick byte = 6
	cmdTouchLongPress   byte = 7
	cmdHardKey          byte = 8

	cmdVRData byte = 1
)

// Touch actions use the Android MotionEvent values.
const (
	motionDown int32 = 0
	motionUp   int32 = 1
	motionMove int32 = 2
)

const (
	frameHeaderSize   = 5
	maxFramePayload   = 1024 * 1024
	reconnectInterval = 2 * time.Second
)

// hostEvent is an event with the frame header already stripped; payload has the tag byte removed.
type hostEvent struct {
	channel byte
	tag     byte
	payload []byte
}

// hostClient maintains the Unix socket connection to carlife-host and reconnects automatically after a drop.
type hostClient struct {
	path string

	writeMu sync.Mutex
	conn    net.Conn

	onEvent func(hostEvent)
	onState func(connected bool)
}

func newHostClient(path string) *hostClient {
	return &hostClient{path: path}
}

// run blocks until ctx is done.
func (h *hostClient) run(ctx context.Context) {
	for ctx.Err() == nil {
		conn, err := net.Dial("unix", h.path)
		if err != nil {
			log.Printf("waiting for carlife-host (%v), retrying in %s", err, reconnectInterval)
			if !sleepContext(ctx, reconnectInterval) {
				return
			}
			continue
		}

		log.Printf("connected to carlife-host: %s", h.path)
		h.writeMu.Lock()
		h.conn = conn
		h.writeMu.Unlock()
		if h.onState != nil {
			h.onState(true)
		}

		h.readLoop(conn)

		h.writeMu.Lock()
		if h.conn == conn {
			h.conn = nil
		}
		h.writeMu.Unlock()
		conn.Close()
		log.Printf("disconnected from carlife-host, reconnecting")
		if h.onState != nil {
			h.onState(false)
		}
	}
}

func (h *hostClient) readLoop(conn net.Conn) {
	header := make([]byte, frameHeaderSize)
	for {
		if _, err := io.ReadFull(conn, header); err != nil {
			return
		}
		channel := header[0]
		length := binary.BigEndian.Uint32(header[1:])
		if length == 0 || length > maxFramePayload {
			log.Printf("carLife-host sent an invalid frame: channel=%d length=%d", channel, length)
			return
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(conn, payload); err != nil {
			return
		}
		if h.onEvent != nil {
			h.onEvent(hostEvent{channel: channel, tag: payload[0], payload: payload[1:]})
		}
	}
}

// send writes one command to carlife-host. It returns an error when the connection is unavailable; the caller decides whether to ignore it.
func (h *hostClient) send(channel, tag byte, payload []byte) error {
	h.writeMu.Lock()
	defer h.writeMu.Unlock()
	if h.conn == nil {
		return fmt.Errorf("carlife-host not connected")
	}
	frame := make([]byte, frameHeaderSize+1+len(payload))
	frame[0] = channel
	binary.BigEndian.PutUint32(frame[1:], uint32(len(payload)+1))
	frame[frameHeaderSize] = tag
	copy(frame[frameHeaderSize+1:], payload)
	if _, err := h.conn.Write(frame); err != nil {
		return err
	}
	return nil
}

// ------------------------------------------------------------ command builders

func be32(value int32) []byte {
	buffer := make([]byte, 4)
	binary.BigEndian.PutUint32(buffer, uint32(value))
	return buffer
}

func (h *hostClient) sendProtocolVersion(major, minor int32) error {
	return h.send(chCmd, cmdProtocolVersion, append(be32(major), be32(minor)...))
}

func (h *hostClient) sendVideoEncoderInit(width, height, fps int32) error {
	payload := append(be32(width), be32(height)...)
	return h.send(chCmd, cmdVideoEncoderInit, append(payload, be32(fps)...))
}

func (h *hostClient) sendVideoEncoderStart() error {
	return h.send(chCmd, cmdVideoEncoderStart, nil)
}

func (h *hostClient) sendGoToForeground() error {
	return h.send(chCmd, cmdGoToForeground, nil)
}

// sendAuthenRequest starts the head unit authentication exchange. The host ignores it
// once the phone has confirmed authentication.
func (h *hostClient) sendAuthenRequest() error {
	return h.send(chCmd, cmdAuthenRequest, nil)
}

func (h *hostClient) sendTouch(action, x, y int32) error {
	payload := append(be32(action), be32(x)...)
	return h.send(chCtrl, cmdTouchAction, append(payload, be32(y)...))
}

func (h *hostClient) sendHardKey(keycode uint32) error {
	return h.send(chCtrl, cmdHardKey, be32(int32(keycode)))
}

func sleepContext(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
