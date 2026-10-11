package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log"
	"strconv"
	"sync"
	"time"
)

const (
	watchdogInterval    = 2 * time.Second
	watchdogIdleTimeout = 5 * time.Second
)

type serverMessage struct {
	Type          string `json:"type"`
	DeviceName    string `json:"deviceName,omitempty"`
	DeviceModel   string `json:"deviceModel,omitempty"`
	SessionActive bool   `json:"sessionActive,omitempty"`
	HostReady     bool   `json:"hostReady,omitempty"`
	Width         int    `json:"width,omitempty"`
	Height        int    `json:"height,omitempty"`
	Message       string `json:"message,omitempty"`
}

// clientMessage is a JSON control message sent by the browser.
type clientMessage struct {
	Type   string  `json:"type"`
	X      float64 `json:"x"`
	Y      float64 `json:"y"`
	Down   bool    `json:"down"`
	Key    string  `json:"key"`
	Width  int     `json:"width"`
	Height int     `json:"height"`
	FPS    int     `json:"fps"`
}

// session tracks CarLife session state: handshake progression, audio/video forwarding, and dispatch of browser commands.
type session struct {
	host *hostClient
	hub  *Hub

	mu sync.Mutex

	hostReady   bool
	deviceName  string
	deviceModel string

	requestedWidth  int
	requestedHeight int
	requestedFPS    int

	width  int
	height int

	// Parameter sets as they were last seen. The phone may deliver SPS and PPS in the
	// same frame or in separate ones, so both are remembered independently and the
	// decoder configuration is rebuilt once both are known.
	sps  []byte
	pps  []byte
	avcC []byte

	streaming bool

	touchDown bool

	audioSampleRate map[byte]int
	audioChannels   map[byte]int
	audioSamples    map[byte]uint64

	/* Timestamp of the last upstream progress, used to re-drive a stalled handshake. */
	lastProgress time.Time
}

func newSession(host *hostClient, hub *Hub, width, height, fps int) *session {
	return &session{
		host:            host,
		hub:             hub,
		requestedWidth:  width,
		requestedHeight: height,
		requestedFPS:    fps,
		audioSampleRate: make(map[byte]int),
		audioChannels:   make(map[byte]int),
		audioSamples:    make(map[byte]uint64),
		lastProgress:    time.Now(),
	}
}

// resetStream drops the per-session progress state. Called whenever carlife-host
// (re)connects, because a reconnect means the previous CarLife session is over: the
// head-unit process was restarted, which only happens after the phone was (re)plugged.
//
// The encoder configuration (sps / pps / avcC / width / height) is deliberately kept. A
// link drop does not necessarily produce a new one: the phone keeps encoding across the
// drop, so it never repeats SPS/PPS. Throwing the known configuration away would leave
// the browser with no decoder configuration at all, and video frames would be useless
// even though they keep arriving.
func (s *session) resetStream() {
	s.mu.Lock()
	s.streaming = false
	s.touchDown = false
	s.audioSampleRate = make(map[byte]int)
	s.audioChannels = make(map[byte]int)
	s.audioSamples = make(map[byte]uint64)
	s.lastProgress = time.Now()
	s.mu.Unlock()
}

// sessionWatchdog re-sends the opening message when the handshake makes no progress.
// The phone never speaks first, and the CarLife app on the phone is launched by the
// system only after the accessory attaches, so the first request can arrive before the
// app is listening (typical when the phone was plugged in after this service started or
// was replugged). Re-sending is harmless: the phone simply answers again.
func (s *session) sessionWatchdog(ctx context.Context) {
	ticker := time.NewTicker(watchdogInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
		s.mu.Lock()
		idle := time.Since(s.lastProgress)
		stalled := s.hostReady && !s.streaming && idle >= watchdogIdleTimeout
		if stalled {
			// Space the retries out instead of firing on every tick.
			s.lastProgress = time.Now()
		}
		s.mu.Unlock()
		if !stalled {
			continue
		}
		log.Printf("no CarLife session yet (idle %s), resending protocol version", idle.Truncate(time.Second))
		s.startHandshake()
	}
}

/* ------------------------------------------------------------ carlife-host events */

func (s *session) onHostEvent(event hostEvent) {
	s.mu.Lock()
	s.lastProgress = time.Now()
	s.mu.Unlock()
	switch event.channel {
	case chCmd:
		s.onCommandEvent(event)
	case chVideo:
		if event.tag == evVideoData {
			s.onVideoData(event.payload)
		}
	case chAudio:
		s.onAudioEvent(audioStreamMedia, streamNameMedia, event)
	case chTTS:
		s.onAudioEvent(audioStreamTTS, streamNameTTS, event)
	case chVR:
		// Voice upstream is pushed by the phone on its own; this process only forwards it, so no extra handling is needed.
	}
}

func (s *session) onCommandEvent(event hostEvent) {
	switch event.tag {
	case evStatus:
		connected := len(event.payload) >= 1 && event.payload[0] == 1
		result := int32(0)
		if len(event.payload) >= 5 {
			result = int32(binary.BigEndian.Uint32(event.payload[1:]))
		}
		s.mu.Lock()
		s.hostReady = connected
		s.mu.Unlock()
		if connected {
			log.Printf("head unit channel ready, starting CarLife handshake")
			s.startHandshake()
		} else {
			log.Printf("head unit channel unavailable (connectionSetup returned %d); make sure usb-bridge is running, then restart carlife-host", result)
		}
		s.broadcastState()

	case evProtocolMatch:
		if len(event.payload) >= 4 {
			log.Printf("protocol version match status: %d", binary.BigEndian.Uint32(event.payload))
		}
		s.requestVideo()

	case evVideoInitDone:
		if len(event.payload) < 12 {
			return
		}
		width := int(binary.BigEndian.Uint32(event.payload))
		height := int(binary.BigEndian.Uint32(event.payload[4:]))
		fps := int(binary.BigEndian.Uint32(event.payload[8:]))
		s.mu.Lock()
		s.width, s.height = width, height
		s.mu.Unlock()
		log.Printf("phone ready, video %dx%d@%d, starting encoder", width, height, fps)
		if err := s.host.sendVideoEncoderStart(); err != nil {
			log.Printf("failed to start video encoder: %v", err)
		}
		s.broadcastState()

	case evMDInfo:
		s.applyDeviceInfo(event.payload)

	case evForeground:
		log.Printf("CarLife on phone moved to foreground")
		s.broadcastState()

	case evBackground:
		log.Printf("CarLife on phone moved to background")

	case evScreenOn:
		log.Printf("phone screen on")

	case evScreenOff:
		log.Printf("phone screen off")

	case evTeleState:
		if len(event.payload) >= 1 {
			log.Printf("call state changed: %d", event.payload[0])
		}

	case evMDExit:
		log.Printf("CarLife on phone exited")
		s.mu.Lock()
		s.streaming = false
		s.avcC = nil
		s.mu.Unlock()
		s.broadcastState()

	case evMicWakeup:
		log.Printf("voice wakeup")

	case evMicEnd:
		log.Printf("voice recording ended")

	case evMicRecog:
		log.Printf("voice recognition in progress")

	case evFeatureConfigRequest:
		log.Printf("replied to phone feature config request")

	case evModuleStatus:
		log.Printf("phone module status: %s", string(event.payload))

	case evLog:
		log.Printf("carlife-host: %s", string(event.payload))
	}
}

func (s *session) applyDeviceInfo(payload []byte) {
	var info map[string]any
	if err := json.Unmarshal(payload, &info); err != nil {
		log.Printf("failed to parse phone device info: %v", err)
		return
	}
	name := textField(info, "model")
	if name == "" {
		name = textField(info, "device")
	}
	s.mu.Lock()
	s.deviceName = name
	s.deviceModel = textField(info, "brand")
	s.mu.Unlock()
	log.Printf("phone device: %s (%s, Android %s)", name, textField(info, "manufacturer"), textField(info, "release"))
	s.broadcastState()
}

// textField reads one device-info value as text. The phone sends a mix of types: most
// fields are strings, but sdk_int is a number, and decoding the payload into a
// map[string]string fails on it and loses the whole record.
func textField(info map[string]any, key string) string {
	switch value := info[key].(type) {
	case string:
		return value
	case float64:
		return strconv.FormatFloat(value, 'f', -1, 64)
	case nil:
		return ""
	default:
		return fmt.Sprintf("%v", value)
	}
}

func (s *session) startHandshake() {
	if err := s.host.sendProtocolVersion(1, 0); err != nil {
		log.Printf("failed to send protocol version: %v", err)
	}
}

func (s *session) requestVideo() {
	s.mu.Lock()
	width, height, fps := s.requestedWidth, s.requestedHeight, s.requestedFPS
	s.mu.Unlock()
	log.Printf("requesting phone to encode video at %dx%d@%d", width, height, fps)
	if err := s.host.sendVideoEncoderInit(int32(width), int32(height), int32(fps)); err != nil {
		log.Printf("failed to request video encoding: %v", err)
	}
}

/* ---------------------------------------------------------------------- video */

func (s *session) onVideoData(data []byte) {
	sps, pps, keyframe := scanAnnexB(data)

	s.mu.Lock()
	if sps != nil {
		s.sps = sps
	}
	if pps != nil {
		s.pps = pps
	}
	changed := false
	if s.sps != nil && s.pps != nil {
		if record := buildAvcC(s.sps, s.pps); record != nil && !bytes.Equal(record, s.avcC) {
			s.avcC = record
			changed = true
		}
	}
	first := !s.streaming
	s.streaming = true
	width, height := s.width, s.height
	if width <= 0 || height <= 0 {
		// The encoder-init-done message never arrived (possible after a link rebuild), so
		// fall back to the mode that was requested. The browser corrects the canvas from
		// the decoded frame anyway.
		width, height = s.requestedWidth, s.requestedHeight
	}
	// A new streaming session needs the configuration again: the browser dropped its
	// decoder when the session went inactive. `changed` covers an encoder restart, which
	// arrives with fresh parameter sets.
	var config []byte
	if s.avcC != nil && (changed || first) {
		config = encodeVideoConfig(videoStreamType, width, height, s.avcC)
	}
	s.mu.Unlock()

	if config != nil {
		log.Printf("video config: %dx%d, %d-byte AVCDecoderConfigurationRecord", width, height, len(config)-11)
		s.hub.BroadcastVideoConfig(config)
	}
	s.hub.BroadcastVideoFrame(keyframe, encodeVideoFrame(videoStreamType, keyframe, data))

	if first {
		if config == nil {
			log.Printf("video frames have started, waiting for SPS/PPS")
		}
		log.Printf("video stream started")
		s.broadcastState()
	}
}

/* ---------------------------------------------------------------------- audio */

func (s *session) onAudioEvent(streamType byte, name string, event hostEvent) {
	switch event.tag {
	case evAudioInit:
		if len(event.payload) < 12 {
			return
		}
		sampleRate := int(binary.BigEndian.Uint32(event.payload))
		channelConfig := int(binary.BigEndian.Uint32(event.payload[4:]))
		channels := channelsFromConfig(channelConfig)
		if sampleRate <= 0 {
			sampleRate = 48000
		}
		s.mu.Lock()
		s.audioSampleRate[streamType] = sampleRate
		s.audioChannels[streamType] = channels
		s.audioSamples[streamType] = 0
		s.mu.Unlock()
		log.Printf("channel %s audio: %d Hz / %d channels", name, sampleRate, channels)
		s.hub.BroadcastAudioConfig(streamType, encodeAudioConfig(streamType, sampleRate, channels, name, nil))

	case evAudioData:
		if len(event.payload) == 0 {
			return
		}
		s.mu.Lock()
		sampleRate := s.audioSampleRate[streamType]
		channels := s.audioChannels[streamType]
		timestampUs := uint64(0)
		if sampleRate > 0 && channels > 0 {
			timestampUs = s.audioSamples[streamType] * 1_000_000 / uint64(sampleRate)
			s.audioSamples[streamType] += uint64(len(event.payload)) / 2 / uint64(channels)
		}
		s.mu.Unlock()
		s.hub.BroadcastAudioPacket(encodeAudioPacket(streamType, timestampUs, event.payload))

	case evAudioStop:
		s.hub.BroadcastAudioStopped(streamType, encodeAudioStopped(streamType))

	case evAudioPause, evAudioResume, evAudioSeek:
		// CarLife playback control only affects the phone side; no extra action is needed here.
	}
}

// channelsFromConfig maps CarLife's channelConfig to a channel count.
// The value may be a simple count or an Android AudioFormat channel mask.
func channelsFromConfig(config int) int {
	switch config {
	case 1, 2:
		return config
	case 4: // CHANNEL_OUT_MONO
		return 1
	case 12: // CHANNEL_OUT_STEREO
		return 2
	default:
		return 2
	}
}

/* ------------------------------------------------------------ browser command handling */

var hardKeyCodes = map[string]uint32{
	"home":    0x00000001,
	"call":    0x00000002,
	"end":     0x00000003,
	"next":    0x00000006,
	"prev":    0x00000007,
	"setting": 0x00000008,
	"media":   0x00000009,
	"radio":   0x0000000A,
	"navi":    0x0000000B,
	"src":     0x0000000C,
	"mode":    0x0000000D,
	"back":    0x0000000E,
	"seekSub": 0x0000000F,
	"seekAdd": 0x00000010,
	"volSub":  0x00000011,
	"volAdd":  0x00000012,
	"mute":    0x00000013,
	"ok":      0x00000014,
	"left":    0x00000015,
	"right":   0x00000016,
	"up":      0x00000017,
	"down":    0x00000018,
	"tel":     0x0000001D,
	"main":    0x0000001E,
}

func (s *session) handleClientMessage(data []byte) {
	var message clientMessage
	if err := json.Unmarshal(data, &message); err != nil {
		return
	}
	switch message.Type {
	case "touch":
		s.handleTouch(message)
	case "key":
		if code, ok := hardKeyCodes[message.Key]; ok {
			if err := s.host.sendHardKey(code); err != nil {
				log.Printf("failed to send hard key %s: %v", message.Key, err)
			}
		}
	case "foreground":
		_ = s.host.sendGoToForeground()
	case "display":
		s.handleDisplayChange(message)
	}
}

func (s *session) handleTouch(message clientMessage) {
	s.mu.Lock()
	width, height := s.width, s.height
	wasDown := s.touchDown
	s.touchDown = message.Down
	s.mu.Unlock()

	if width <= 0 || height <= 0 {
		return
	}

	var action int32
	switch {
	case message.Down && !wasDown:
		action = motionDown
	case message.Down && wasDown:
		action = motionMove
	case !message.Down && wasDown:
		action = motionUp
	default:
		return
	}

	x := int32(clampFloat(message.X, 0, 1) * float64(width))
	y := int32(clampFloat(message.Y, 0, 1) * float64(height))
	if x >= int32(width) {
		x = int32(width) - 1
	}
	if y >= int32(height) {
		y = int32(height) - 1
	}
	if err := s.host.sendTouch(action, x, y); err != nil {
		log.Printf("failed to send touch: %v", err)
	}
}

func (s *session) handleDisplayChange(message clientMessage) {
	if message.Width <= 0 || message.Height <= 0 {
		return
	}
	fps := message.FPS
	if fps <= 0 {
		fps = s.requestedFPS
	}
	s.mu.Lock()
	s.requestedWidth, s.requestedHeight, s.requestedFPS = message.Width, message.Height, fps
	s.mu.Unlock()

	log.Printf("switching video mode to %dx%d@%d", message.Width, message.Height, fps)
	go func() {
		// Reset the encoder first, then renegotiate with the new mode; otherwise the phone will not start producing frames again.
		_ = s.host.send(chCmd, cmdVideoEncoderReset, nil)
		time.Sleep(300 * time.Millisecond)
		s.mu.Lock()
		s.streaming = false
		s.avcC = nil
		s.mu.Unlock()
		_ = s.host.sendVideoEncoderInit(int32(message.Width), int32(message.Height), int32(fps))
	}()
}

/* ---------------------------------------------------------------------- broadcast */

func (s *session) broadcastState() {
	s.mu.Lock()
	message := serverMessage{
		Type:          "session",
		DeviceName:    s.deviceName,
		DeviceModel:   s.deviceModel,
		SessionActive: s.streaming,
		HostReady:     s.hostReady,
		Width:         s.width,
		Height:        s.height,
	}
	s.mu.Unlock()
	s.hub.BroadcastJSON(message)
}

func clampFloat(value, low, high float64) float64 {
	if value < low {
		return low
	}
	if value > high {
		return high
	}
	return value
}
