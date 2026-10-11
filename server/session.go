package main

import (
	"encoding/binary"
	"encoding/json"
	"log"
	"sync"
	"time"
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

	avcC      []byte
	streaming bool

	touchDown bool

	audioSampleRate map[byte]int
	audioChannels   map[byte]int
	audioSamples    map[byte]uint64
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
	}
}

/* ------------------------------------------------------------ carlife-host events */

func (s *session) onHostEvent(event hostEvent) {
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
		log.Printf("carlife-host：%s", string(event.payload))
	}
}

func (s *session) applyDeviceInfo(payload []byte) {
	var info map[string]string
	if err := json.Unmarshal(payload, &info); err != nil {
		log.Printf("failed to parse phone device info: %v", err)
		return
	}
	name := info["model"]
	if name == "" {
		name = info["device"]
	}
	s.mu.Lock()
	s.deviceName = name
	s.deviceModel = info["brand"]
	s.mu.Unlock()
	log.Printf("phone device: %s (%s, Android %s)", name, info["manufacturer"], info["release"])
	s.broadcastState()
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

	var config []byte
	s.mu.Lock()
	if sps != nil && pps != nil {
		if record := buildAvcC(sps, pps); record != nil && string(record) != string(s.avcC) {
			s.avcC = record
			config = encodeVideoConfig(videoStreamType, s.width, s.height, record)
		}
	}
	first := !s.streaming
	s.streaming = true
	s.mu.Unlock()

	if config != nil {
		s.hub.Broadcast(config)
	}
	s.hub.Broadcast(encodeVideoFrame(videoStreamType, keyframe, data))

	if first {
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
		s.hub.Broadcast(encodeAudioConfig(streamType, sampleRate, channels, name, nil))

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
		s.hub.Broadcast(encodeAudioPacket(streamType, timestampUs, event.payload))

	case evAudioStop:
		s.hub.Broadcast(encodeAudioStopped(streamType))

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
