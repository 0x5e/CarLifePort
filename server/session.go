package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	watchdogInterval    = 2 * time.Second
	watchdogIdleTimeout = 5 * time.Second
	// The request can be missed -- the phone app may not be listening yet when it is sent --
	// and the phone's verdict only ever follows a request, so the exchange is repeated until
	// the phone reports one.
	authenRetryInterval = 30 * time.Second
	// Window over which the received video frame rate is measured.
	fpsReportInterval = 5 * time.Second
)

// clientMessage is the JSON control message the playport web client sends. It mirrors
// ClientMessage in WebHub.kt: CarLife has no knob, no Siri and no night mode, so those
// fields are translated to the closest CarLife hard key or dropped.
type clientMessage struct {
	Type      string         `json:"type"`
	Contacts  []touchContact `json:"contacts"`
	Media     *int           `json:"media"`
	Knob      *knobState     `json:"knob"`
	Night     *bool          `json:"night"`
	Telephony *int           `json:"telephony"`
}

type touchContact struct {
	ID   int     `json:"id"`
	X    float64 `json:"x"`
	Y    float64 `json:"y"`
	Down bool    `json:"down"`
}

type knobState struct {
	Select bool `json:"select"`
	Home   bool `json:"home"`
	Back   bool `json:"back"`
	X      int  `json:"x"`
	Y      int  `json:"y"`
	Wheel  int  `json:"wheel"`
}

// session tracks CarLife session state: handshake progression, audio/video forwarding, and dispatch of browser commands.
type session struct {
	link *linkClient
	hub  *Hub

	mu sync.Mutex

	linkReady   bool
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

	// Head unit authentication: the phone warns and eventually disconnects an
	// unauthenticated head unit, so the exchange is driven from here until the phone
	// confirms.
	authenDone    bool
	lastAuthenTry time.Time

	// Received video frames over the current measurement window, reported periodically
	// to tell a phone-side frame rate limit apart from frames lost on our side.
	frameCount int
	frameBytes int
	fpsWindow  time.Time

	touchDown bool
	// Where the pointer was last reported, in device pixels. The client lists a finger only
	// while it is down, so a lift arrives as an empty contact list; a release sent at the
	// origin instead would report the finger jumping to the top left corner, and the phone
	// cancels a press that leaves the control before it lifts.
	touchX int32
	touchY int32

	// The client has one play/pause button where CarLife has a key for each, so the last
	// state this client asked for decides which of the two the next press sends.
	mediaPlaying bool
	// The same for the one voice button, which opens and closes the recognition screen.
	voiceActive bool
	// Last call state the phone reported, which decides whether the single call button
	// means "answer" or "hang up".
	callState int

	audioSampleRate map[byte]int
	audioChannels   map[byte]int
	audioSamples    map[byte]uint64

	/* Timestamp of the last upstream progress, used to re-drive a stalled handshake. */
	lastProgress time.Time
}

func newSession(link *linkClient, hub *Hub, width, height, fps int) *session {
	return &session{
		link:            link,
		hub:             hub,
		requestedWidth:  width,
		requestedHeight: height,
		requestedFPS:    fps,
		audioSampleRate: make(map[byte]int),
		audioChannels:   make(map[byte]int),
		audioSamples:    make(map[byte]uint64),
		lastProgress:    time.Now(),
		fpsWindow:       time.Now(),
	}
}

// resetStream drops the per-session progress state. Called whenever carlife-link
// (re)connects, because a reconnect means the previous CarLife session is over: the link
// process was restarted, which only happens after the phone was (re)plugged.
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
	s.authenDone = false
	s.frameCount = 0
	s.frameBytes = 0
	s.fpsWindow = time.Now()
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
		stalled := s.linkReady && !s.streaming && idle >= watchdogIdleTimeout
		if stalled {
			// Space the retries out instead of firing on every tick.
			s.lastProgress = time.Now()
		}
		retryAuthen := s.linkReady && !s.authenDone && time.Since(s.lastAuthenTry) >= authenRetryInterval
		s.mu.Unlock()
		if stalled {
			warnf(moduleSession, "no CarLife session yet (idle %s), resending protocol version", idle.Truncate(time.Second))
			s.startHandshake()
		}
		if retryAuthen {
			s.requestAuthentication()
		}
		s.reportFrameRate()
	}
}

// requestAuthentication has carlife-link run the head unit authentication exchange
// (guide 17.1): it sends MSG_CMD_HU_AUTHEN_REQUEST, checks the phone's signed answer
// against the value it sent, and reports the result back. Repeating it is harmless, and
// useful after a phone restart: carlife-link skips the request once the phone has
// confirmed, and a fresh exchange is answered the same way.
//
// The two directions of the authentication are independent. This one identifies the phone
// to the head unit; the phone goes the other way and decides from the channel number in the
// statistics info whether the head unit is legal (guide 17.2), which is the verdict that
// arrives here as evAuthenResult.
func (s *session) requestAuthentication() {
	s.mu.Lock()
	s.lastAuthenTry = time.Now()
	s.mu.Unlock()
	if err := s.link.sendAuthenRequest(); err != nil {
		errorf(moduleSession, "failed to start head unit authentication: %v", err)
		return
	}
	infof(moduleSession, "running head unit authentication exchange (retry every %s until the phone confirms)",
		authenRetryInterval)
}

// reportFrameRate logs the measured rate of the video frames arriving from the phone,
// which is what tells a phone-side limit apart from frames lost on this side.
func (s *session) reportFrameRate() {
	s.mu.Lock()
	elapsed := time.Since(s.fpsWindow)
	if !s.streaming || elapsed < fpsReportInterval {
		s.mu.Unlock()
		return
	}
	frames, total, requested := s.frameCount, s.frameBytes, s.requestedFPS
	s.frameCount, s.frameBytes, s.fpsWindow = 0, 0, time.Now()
	s.mu.Unlock()

	if frames == 0 {
		debugf(moduleSession, "video: no frames in the last %s", elapsed.Truncate(time.Second))
		return
	}
	debugf(moduleSession, "video: %.1f fps received (%d frames, %.1f KB/s) in the last %s, requested %d fps",
		float64(frames)/elapsed.Seconds(), frames, float64(total)/1024/elapsed.Seconds(), elapsed.Truncate(time.Second), requested)
}

/* ------------------------------------------------------------ carlife-link events */

func (s *session) onLinkEvent(event linkEvent) {
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

func (s *session) onCommandEvent(event linkEvent) {
	switch event.tag {
	case evStatus:
		connected := len(event.payload) >= 1 && event.payload[0] == 1
		result := int32(0)
		if len(event.payload) >= 5 {
			result = int32(binary.BigEndian.Uint32(event.payload[1:]))
		}
		s.mu.Lock()
		s.linkReady = connected
		s.mu.Unlock()
		if connected {
			infof(moduleSession, "head unit channel ready, starting CarLife handshake")
			s.startHandshake()
		} else {
			warnf(moduleSession, "head unit channel unavailable (the link reported %d), it will be rebuilt", result)
		}
		s.broadcastState()

	case evProtocolMatch:
		if len(event.payload) >= 4 {
			infof(moduleSession, "protocol version match status: %d", binary.BigEndian.Uint32(event.payload))
		}
		s.requestVideo()
		// The guide has the head unit start the authentication once the versions match,
		// which is here.
		s.requestAuthentication()

	case evVideoInitDone:
		if len(event.payload) < 12 {
			return
		}
		width := int(binary.BigEndian.Uint32(event.payload))
		height := int(binary.BigEndian.Uint32(event.payload[4:]))
		fps := int(binary.BigEndian.Uint32(event.payload[8:]))
		s.mu.Lock()
		s.width, s.height = width, height
		requestedFPS := s.requestedFPS
		s.mu.Unlock()
		infof(moduleSession, "phone ready, video %dx%d@%d, starting encoder", width, height, fps)
		if fps > 0 && fps < requestedFPS {
			warnf(moduleSession, "phone limited the frame rate to %d fps (requested %d)", fps, requestedFPS)
		}
		if err := s.link.sendVideoEncoderStart(); err != nil {
			errorf(moduleSession, "failed to start video encoder: %v", err)
		}
		s.broadcastState()

	case evMDInfo:
		s.applyDeviceInfo(event.payload)

	case evForeground:
		infof(moduleSession, "CarLife on phone moved to foreground")
		s.broadcastState()

	case evBackground:
		infof(moduleSession, "CarLife on phone moved to background")

	case evScreenOn:
		infof(moduleSession, "phone screen on")

	case evScreenOff:
		infof(moduleSession, "phone screen off")

	case evTeleState:
		if len(event.payload) >= 1 {
			s.mu.Lock()
			s.callState = int(event.payload[0])
			s.mu.Unlock()
			infof(moduleSession, "call state changed: %d", event.payload[0])
		}

	case evMDExit:
		infof(moduleSession, "CarLife on phone exited")
		s.mu.Lock()
		s.streaming = false
		s.avcC = nil
		s.mu.Unlock()
		s.broadcastState()

	case evMicWakeup:
		infof(moduleSession, "voice wakeup")

	case evMicEnd:
		infof(moduleSession, "voice recording ended")

	case evMicRecog:
		infof(moduleSession, "voice recognition in progress")

	case evAuthenResult:
		passed := len(event.payload) >= 1 && event.payload[0] == 1
		s.mu.Lock()
		s.authenDone = passed
		s.mu.Unlock()
		if passed {
			infof(moduleSession, "phone authenticated this head unit")
		} else {
			warnf(moduleSession, "phone rejected this head unit; retrying authentication in %s", authenRetryInterval)
			// A rejection leaves a "head unit not authorized" dialog on the phone that
			// waits for a tap before CarLife can be used. Sending the home key dismisses
			// that dialog, so a rejected session gets past it without anyone touching the
			// phone. The rejection itself is unaffected; this only closes the dialog.
			s.sendHardKey(keyMain)
		}

	case evFeatureConfigRequest:
		infof(moduleSession, "replied to phone feature config request")

	case evModuleStatus:
		infof(moduleSession, "phone module status: %s", describeModuleStatus(event.payload))

	case evLog:
		debugf(moduleSession, "carlife-link: %s", string(event.payload))
	}
}

// describeModuleStatus renders the module status the phone reports. The raw form is a JSON
// list of {moduleID, statusID} pairs whose meaning is only in the module id names, and one
// entry decides whether audio can arrive at all: module 3 (music) reports "running" only
// while the phone is streaming program audio, so an idle music module means the phone has
// nothing to play and no audio channel traffic is expected.
func describeModuleStatus(payload []byte) string {
	var entries []struct {
		ModuleID int `json:"moduleID"`
		StatusID int `json:"statusID"`
	}
	if err := json.Unmarshal(payload, &entries); err != nil {
		return string(payload)
	}
	parts := make([]string, 0, len(entries))
	for _, entry := range entries {
		parts = append(parts, moduleName(entry.ModuleID)+"="+moduleStatusName(entry.ModuleID, entry.StatusID))
	}
	return strings.Join(parts, " ")
}

// Module ids and status ids come from ModuleStatusModel.java in the CarLife vehicle library.
func moduleName(moduleID int) string {
	switch moduleID {
	case 1:
		return "phone"
	case 2:
		return "navi"
	case 3:
		return "music"
	case 4:
		return "voice"
	case 5:
		return "connect"
	case 6:
		return "mic"
	}
	return fmt.Sprintf("module%d", moduleID)
}

func moduleStatusName(moduleID, statusID int) string {
	switch moduleID {
	case 1: // phone
		return statusName(statusID, "idle", "incoming", "outgoing")
	case 2: // navi
		return statusName(statusID, "idle", "start", "stop")
	case 3: // music
		return statusName(statusID, "idle", "running")
	case 4: // voice
		return statusName(statusID, "idle", "running", "mic-unsupported")
	case 6: // mic
		return statusName(statusID, "vehicle-mic", "phone-mic", "unsupported")
	}
	return strconv.Itoa(statusID)
}

// statusName picks the name for a status id, falling back to the number when the phone
// reports one this build does not know.
func statusName(statusID int, names ...string) string {
	if statusID >= 0 && statusID < len(names) {
		return names[statusID]
	}
	return strconv.Itoa(statusID)
}

func (s *session) applyDeviceInfo(payload []byte) {
	var info map[string]any
	if err := json.Unmarshal(payload, &info); err != nil {
		warnf(moduleSession, "failed to parse phone device info: %v", err)
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
	infof(moduleSession, "phone device: %s (%s, Android %s)", name, textField(info, "manufacturer"), textField(info, "release"))
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
	if err := s.link.sendProtocolVersion(1, 0); err != nil {
		errorf(moduleSession, "failed to send protocol version: %v", err)
	}
}

func (s *session) requestVideo() {
	s.mu.Lock()
	width, height, fps := s.requestedWidth, s.requestedHeight, s.requestedFPS
	s.mu.Unlock()
	infof(moduleSession, "requesting phone to encode video at %dx%d@%d", width, height, fps)
	if err := s.link.sendVideoEncoderInit(int32(width), int32(height), int32(fps)); err != nil {
		errorf(moduleSession, "failed to request video encoding: %v", err)
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
	if first {
		// Measure from the first frame; otherwise the first window covers the idle time
		// before the stream started and reports a rate that is too low.
		s.frameCount = 0
		s.frameBytes = 0
		s.fpsWindow = time.Now()
	}
	s.frameCount++
	s.frameBytes += len(data)
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
		infof(moduleSession, "video config: %dx%d, %d-byte AVCDecoderConfigurationRecord", width, height, len(config)-11)
		s.hub.BroadcastVideoConfig(config)
	}
	s.hub.BroadcastVideoFrame(keyframe, encodeVideoFrame(videoStreamType, keyframe, data))

	if first {
		if config == nil {
			warnf(moduleSession, "video frames have started, waiting for SPS/PPS")
		}
		infof(moduleSession, "video stream started")
		s.broadcastState()
	}
}

/* ---------------------------------------------------------------------- audio */

func (s *session) onAudioEvent(streamType byte, name string, event linkEvent) {
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
		infof(moduleSession, "channel %s audio: %d Hz / %d channels", name, sampleRate, channels)
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

// CarLife hard key codes, table 10.6-1 of the protocol guide. The reference head unit
// numbers them the same way (CommonParams.KEYCODE_*) and its control panel sends exactly
// one of these per button (ControlTestWindow).
const (
	keyPhoneCall    = 0x00000002 // place or answer a call
	keyPhoneEnd     = 0x00000003 // hang up
	keySelectorNext = 0x00000006 // focus the next item in the current region
	keySelectorPrev = 0x00000007 // focus the previous item
	keyBack         = 0x0000000E // back to the previous CarLife screen
	keySeekSub      = 0x0000000F // previous track
	keySeekAdd      = 0x00000010 // next track
	keyMute         = 0x00000013 // mute the microphone while on a call
	keyOK           = 0x00000014 // confirm the focused item
	keyMoveLeft     = 0x00000015 // focus the region to the left
	keyMoveRight    = 0x00000016 // focus the region to the right
	keyMain         = 0x0000001E // the CarLife home screen
	keyMediaStart   = 0x0000001F // play
	keyMediaStop    = 0x00000020 // pause
	keyVRStart      = 0x00000021 // open the voice recognition screen
	keyVRStop       = 0x00000022 // close it
)

// Call states the phone reports over EV_TELE_STATE.
const (
	callStateIdle = iota
	callStateIncoming
	callStateOutgoing
	callStateInCall
)

func (s *session) handleClientMessage(data []byte) {
	var message clientMessage
	if err := json.Unmarshal(data, &message); err != nil {
		return
	}
	switch message.Type {
	case "touch":
		s.handleTouch(message.Contacts)
	case "knob":
		if message.Knob != nil {
			s.handleKnob(*message.Knob)
		}
	case "media":
		if message.Media != nil {
			s.handleMedia(*message.Media)
		}
	case "keyframe":
		// CarLife has no way to ask the phone for a keyframe; the encoder restarts on
		// its own GOP, so the request is dropped.
	case "night":
		// CarPlay-only control with no CarLife equivalent.
	case "siri":
		s.handleSiri()
	case "telephony":
		if message.Telephony != nil {
			s.handleTelephony(*message.Telephony)
		}
	default:
	}
}

// handleTouch maps the playport contact list onto CarLife's single-pointer protocol.
// An empty contact list means every finger lifted, which is the UP event.
func (s *session) handleTouch(contacts []touchContact) {
	s.mu.Lock()
	width, height := s.width, s.height
	if width <= 0 || height <= 0 {
		width, height = s.requestedWidth, s.requestedHeight
	}
	wasDown := s.touchDown
	lastX, lastY := s.touchX, s.touchY
	s.mu.Unlock()

	if width <= 0 || height <= 0 {
		return
	}

	if len(contacts) == 0 {
		if !wasDown {
			return
		}
		s.mu.Lock()
		s.touchDown = false
		s.mu.Unlock()
		// The lift carries no position of its own: the client drops the contact instead of
		// sending one with down clear, so the release repeats the last one. Naming the origin
		// here reads as the finger jumping to the top left corner, and the phone cancels a
		// press that leaves the control before it lifts instead of completing it.
		if err := s.link.sendTouch(motionUp, lastX, lastY); err != nil {
			errorf(moduleSession, "failed to send touch release: %v", err)
		}
		return
	}

	contact := contacts[0]
	for _, other := range contacts[1:] {
		if other.ID < contact.ID {
			contact = other
		}
	}

	var action int32 = motionMove
	if !wasDown {
		action = motionDown
	}

	x := int32(clampFloat(contact.X, 0, 1) * float64(width))
	y := int32(clampFloat(contact.Y, 0, 1) * float64(height))
	if x >= int32(width) {
		x = int32(width) - 1
	}
	if y >= int32(height) {
		y = int32(height) - 1
	}

	s.mu.Lock()
	s.touchDown = true
	s.touchX = x
	s.touchY = y
	s.mu.Unlock()

	if err := s.link.sendTouch(action, x, y); err != nil {
		errorf(moduleSession, "failed to send touch: %v", err)
	}
}

// handleKnob translates a knob event from the client into CarLife hard keys. CarLife
// has no knob of its own, so the four directions are emulated with the two families of
// keycodes the protocol does offer: Selector_Previous/Next move the focus within the
// current region (what turning a knob does) and Move_Left/Right jump to the neighbouring
// region. Move_Up/Down exist as well, but the phone ignores them on its list screens while
// it does act on Selector, and Selector has no horizontal counterpart - hence one family
// per axis.
func (s *session) handleKnob(knob knobState) {
	switch {
	case knob.Select:
		s.sendHardKey(keyOK)
	case knob.Home:
		s.sendHardKey(keyMain)
	case knob.Back:
		s.sendHardKey(keyBack)
	case knob.X < 0:
		s.sendHardKey(keyMoveLeft)
	case knob.X > 0:
		s.sendHardKey(keyMoveRight)
	case knob.Y < 0:
		s.sendHardKey(keySelectorPrev)
	case knob.Y > 0:
		s.sendHardKey(keySelectorNext)
	}
}

// handleMedia maps the playport media button indices onto CarLife hard keys: 3 = play or
// pause, 4 = next track, 5 = previous track. The reference head unit routes Android's
// KEYCODE_MEDIA_PREVIOUS/NEXT to Seek_Sub/Add the same way (MediaButtonReceiver) and gives
// play and pause a button each (ControlTestWindow), so the single button here alternates.
func (s *session) handleMedia(index int) {
	switch index {
	case 3:
		s.mu.Lock()
		playing := s.mediaPlaying
		s.mediaPlaying = !playing
		s.mu.Unlock()
		if playing {
			s.sendHardKey(keyMediaStop)
			return
		}
		s.sendHardKey(keyMediaStart)
	case 4:
		s.sendHardKey(keySeekAdd)
	case 5:
		s.sendHardKey(keySeekSub)
	}
}

// handleTelephony maps playport's call buttons onto CarLife hard keys: 1 = answer or end,
// 2 = mute the call microphone. Answering and hanging up are two keys in CarLife, so the
// call state the phone reports decides which one the single button sends.
func (s *session) handleTelephony(action int) {
	switch action {
	case 1:
		s.mu.Lock()
		inCall := s.callState == callStateInCall
		s.mu.Unlock()
		if inCall {
			s.sendHardKey(keyPhoneEnd)
			return
		}
		s.sendHardKey(keyPhoneCall)
	case 2:
		s.sendHardKey(keyMute)
	}
}

// handleSiri opens and closes CarLife's voice recognition screen. The client sends a single
// event per press, but CarLife has a key for each direction, so the two alternate.
//
// Recognition itself needs a microphone, which is a separate gap: the feature configuration
// we report says the vehicle microphone is used (VOICE_MIC=0), and the phone then waits for
// PCM on the VR channel, which we never send.
func (s *session) handleSiri() {
	s.mu.Lock()
	active := !s.voiceActive
	s.voiceActive = active
	s.mu.Unlock()
	if active {
		s.sendHardKey(keyVRStart)
		return
	}
	s.sendHardKey(keyVRStop)
}

func (s *session) sendHardKey(code uint32) {
	if err := s.link.sendHardKey(code); err != nil {
		errorf(moduleSession, "failed to send hard key 0x%02X: %v", code, err)
	}
}

// changeDisplay renegotiates the video mode. It is called from the /api/display handler.
func (s *session) changeDisplay(width, height, fps int) error {
	if width <= 0 || height <= 0 {
		return fmt.Errorf("width and height must be positive")
	}
	if fps <= 0 {
		fps = s.requestedFPS
	}
	s.mu.Lock()
	s.requestedWidth, s.requestedHeight, s.requestedFPS = width, height, fps
	s.mu.Unlock()

	infof(moduleSession, "switching video mode to %dx%d@%d", width, height, fps)
	// Reset the encoder first, then renegotiate with the new mode; otherwise the phone
	// will not start producing frames again.
	_ = s.link.send(chCmd, cmdVideoEncoderReset, nil)
	s.mu.Lock()
	s.streaming = false
	s.avcC = nil
	s.mu.Unlock()
	if err := s.link.sendVideoEncoderInit(int32(width), int32(height), int32(fps)); err != nil {
		return fmt.Errorf("carlife-link is not connected")
	}
	return nil
}

/* ---------------------------------------------------------------------- broadcast */

// isActive reports whether the phone is currently producing video.
func (s *session) isActive() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.streaming
}

// phoneName is the model name reported by the phone, shown as the connected device.
func (s *session) phoneName() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.deviceName
}

func (s *session) broadcastState() {
	s.mu.Lock()
	active := s.streaming
	width, height := s.width, s.height
	if width <= 0 || height <= 0 {
		width, height = s.requestedWidth, s.requestedHeight
	}
	phone := s.deviceName
	s.mu.Unlock()

	s.hub.BroadcastJSON(serverMessage{
		Type:          "session",
		DeviceName:    s.hub.deviceName,
		SessionActive: active,
		Width:         width,
		Height:        height,
		Viewers:       s.hub.count(),
		Message:       phone,
	})
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
