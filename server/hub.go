package main

import (
	"context"
	"encoding/json"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// keyframeWaitFallback bounds how long a viewer is held back while waiting for a
// keyframe. CarLife has no "request keyframe" command, so a viewer that joins in the
// middle of a long GOP can only wait; this keeps the wait finite.
const keyframeWaitFallback = 10 * time.Second

// viewerQueueDepth is the per-viewer send queue depth. It matches SEND_QUEUE_CAPACITY in
// WebHub.kt. The queue is bounded and a viewer that cannot keep up loses the newest
// message (see deliver), so a stalled viewer costs at most this many undelivered frames
// and the live edge is never held back by it.
const viewerQueueDepth = 1024

// serverMessage is the JSON control message the playport web client understands.
// Field names and semantics mirror ServerMessage in WebHub.kt.
type serverMessage struct {
	Type          string `json:"type"`
	DeviceName    string `json:"deviceName,omitempty"`
	SessionActive bool   `json:"sessionActive,omitempty"`
	Width         int    `json:"width,omitempty"`
	Height        int    `json:"height,omitempty"`
	Viewers       int    `json:"viewers"`
	Message       string `json:"message,omitempty"`
}

type outbound struct {
	text bool
	data []byte
}

type viewer struct {
	conn *websocket.Conn
	send chan outbound
	peer string

	// ready is set once a keyframe has been forwarded. A decoder that starts on a delta
	// frame fails outright, so deltas are withheld until then. Guarded by Hub.mu.
	ready       bool
	waitedSince time.Time
}

// Hub manages all browser viewers and forwards their control messages to the upstream session.
type Hub struct {
	mu sync.Mutex

	deviceName string
	display    *displayState
	session    *session

	viewers map[*viewer]struct{}

	// Most recent codec setup per stream, replayed to a viewer that joins after the
	// stream already started. Without it such a viewer receives video frames but never
	// the decoder configuration, so nothing is ever painted.
	videoConfig map[byte][]byte
	audioConfig map[byte][]byte
}

func newHub(deviceName string, display *displayState) *Hub {
	return &Hub{
		deviceName:  deviceName,
		display:     display,
		viewers:     make(map[*viewer]struct{}),
		videoConfig: make(map[byte][]byte),
		audioConfig: make(map[byte][]byte),
	}
}

// Routes builds the HTTP surface: the WebSocket stream, the two JSON endpoints the client
// polls, and the browser client itself, which is served from the assets embedded in this
// binary (see webassets.go).
func (h *Hub) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", h.serveWebSocket)
	mux.HandleFunc("/api/status", h.serveStatus)
	mux.HandleFunc("/api/display", h.serveDisplay)
	mux.Handle("/", http.FileServerFS(webRoot()))
	return mux
}

func (h *Hub) add(v *viewer) {
	h.mu.Lock()
	h.viewers[v] = struct{}{}
	h.mu.Unlock()
}

func (h *Hub) remove(v *viewer) {
	h.mu.Lock()
	delete(h.viewers, v)
	h.mu.Unlock()
}

func (h *Hub) count() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.viewers)
}

func (h *Hub) snapshot() []*viewer {
	h.mu.Lock()
	defer h.mu.Unlock()
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
		viewers = append(viewers, v)
	}
	return viewers
}

// deliver hands one message to every viewer; a viewer that cannot keep up drops it.
func deliver(viewers []*viewer, message outbound) {
	for _, v := range viewers {
		select {
		case v.send <- message:
		default:
		}
	}
}

// BroadcastVideoConfig caches the decoder configuration of a stream and forwards it.
// Every viewer has to be served it again afterwards: the browser rebuilds its decoder on
// each configuration message, and a fresh decoder must start at a keyframe.
func (h *Hub) BroadcastVideoConfig(frame []byte) {
	h.mu.Lock()
	h.videoConfig[videoStreamType] = frame
	now := time.Now()
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
		v.ready = false
		v.waitedSince = now
		viewers = append(viewers, v)
	}
	h.mu.Unlock()
	deliver(viewers, outbound{data: frame})
}

// BroadcastVideoFrame forwards one encoded frame, withholding deltas from viewers that
// have not received a keyframe yet.
func (h *Hub) BroadcastVideoFrame(keyframe bool, frame []byte) {
	now := time.Now()
	h.mu.Lock()
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
		if !v.ready {
			if !keyframe && now.Sub(v.waitedSince) < keyframeWaitFallback {
				continue
			}
			if !keyframe {
				warnf(moduleViewer, "viewer.keyframe_timeout peer=%s waited=%s action=forward_delta",
					v.peer, now.Sub(v.waitedSince).Truncate(time.Second))
			}
			v.ready = true
		}
		viewers = append(viewers, v)
	}
	h.mu.Unlock()
	deliver(viewers, outbound{data: frame})
}

// BroadcastAudioConfig caches the audio setup of a stream and forwards it.
func (h *Hub) BroadcastAudioConfig(streamType byte, frame []byte) {
	h.mu.Lock()
	h.audioConfig[streamType] = frame
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
		viewers = append(viewers, v)
	}
	h.mu.Unlock()
	deliver(viewers, outbound{data: frame})
}

// BroadcastAudioPacket forwards one audio payload.
func (h *Hub) BroadcastAudioPacket(frame []byte) {
	deliver(h.snapshot(), outbound{data: frame})
}

// BroadcastAudioStopped forwards an end-of-stream notice and drops the cached setup.
func (h *Hub) BroadcastAudioStopped(streamType byte, frame []byte) {
	h.mu.Lock()
	delete(h.audioConfig, streamType)
	h.mu.Unlock()
	deliver(h.snapshot(), outbound{data: frame})
}

// BroadcastJSON sends one control message to all viewers.
func (h *Hub) BroadcastJSON(value any) {
	data, err := json.Marshal(value)
	if err != nil {
		return
	}
	deliver(h.snapshot(), outbound{text: true, data: data})
}

func (h *Hub) serveWebSocket(w http.ResponseWriter, r *http.Request) {
	if r.URL.Query().Get("token") != accessToken {
		warnf(moduleViewer, "viewer.connection_rejected peer=%s reason=invalid_token", r.RemoteAddr)
		conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
		if err != nil {
			return
		}
		_ = conn.Close(websocket.StatusPolicyViolation, "invalid token")
		return
	}

	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// Local tool: allow the page to be opened from any origin.
		InsecureSkipVerify: true,
	})
	if err != nil {
		warnf(moduleViewer, "viewer.handshake_failed err=%v", err)
		return
	}

	v := &viewer{
		conn:        conn,
		send:        make(chan outbound, viewerQueueDepth),
		peer:        r.RemoteAddr,
		waitedSince: time.Now(),
	}
	defer func() {
		h.remove(v)
		infof(moduleViewer, "viewer.disconnected peer=%s clients=%d", v.peer, h.count())
	}()
	defer conn.CloseNow()

	// The queue is drained by writeViewer, which has to be running before anything is
	// pushed into it.
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	go writeViewer(ctx, v)

	// Send the handshake and the current codec setup before the viewer takes part in
	// broadcasts, so that no video frame can overtake the configuration it needs.
	h.mu.Lock()
	hello := serverMessage{
		Type:          "hello",
		DeviceName:    h.deviceName,
		SessionActive: h.session.isActive(),
		Width:         h.display.Width,
		Height:        h.display.Height,
		Viewers:       len(h.viewers) + 1,
		Message:       h.session.phoneName(),
	}
	h.mu.Unlock()
	select {
	case v.send <- outbound{text: true, data: mustJSON(hello)}:
	default:
	}

	h.mu.Lock()
	for _, frame := range h.videoConfig {
		select {
		case v.send <- outbound{data: frame}:
		default:
		}
	}
	for _, frame := range h.audioConfig {
		select {
		case v.send <- outbound{data: frame}:
		default:
		}
	}
	h.mu.Unlock()

	h.add(v)
	infof(moduleViewer, "viewer.connected peer=%s clients=%d", v.peer, h.count())
	h.session.broadcastState()

	for {
		_, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		// Anything that is not JSON is microphone uplink (0x10 LPCM / 0x11 Opus).
		// CarLife voice uplink is driven by the phone, not by the head unit, so it is
		// discarded.
		if !isJSONPayload(data) {
			continue
		}
		if messageType(data) == "ping" {
			select {
			case v.send <- outbound{text: true, data: []byte(`{"type":"pong"}`)}:
			default:
			}
			continue
		}
		h.session.handleClientMessage(data)
	}
}

func isJSONPayload(data []byte) bool {
	for _, b := range data {
		if b == ' ' || b == '\n' || b == '\r' || b == '\t' {
			continue
		}
		return b == '{'
	}
	return false
}

func messageType(data []byte) string {
	var probe struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal(data, &probe); err != nil {
		return ""
	}
	return probe.Type
}

func writeViewer(ctx context.Context, v *viewer) {
	for {
		select {
		case <-ctx.Done():
			return
		case message := <-v.send:
			kind := websocket.MessageBinary
			if message.text {
				kind = websocket.MessageText
			}
			writeCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
			err := v.conn.Write(writeCtx, kind, message.data)
			cancel()
			if err != nil {
				return
			}
		}
	}
}

func mustJSON(value any) []byte {
	data, err := json.Marshal(value)
	if err != nil {
		return []byte("{}")
	}
	return data
}

/* ----------------------------------------------------------------- status API */

func (h *Hub) serveStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	h.mu.Lock()
	payload := map[string]any{
		"deviceName":    h.deviceName,
		"sessionActive": h.session.isActive(),
		"phone":         h.session.phoneName(),
		"viewers":       len(h.viewers),
		"display": map[string]any{
			"width":       h.display.Width,
			"height":      h.display.Height,
			"fps":         h.display.FPS,
			"uiScale":     h.display.UIScale,
			"hevc":        h.display.HEVC,
			"orientation": h.display.orientation(),
		},
	}
	h.mu.Unlock()
	writeJSON(w, http.StatusOK, payload)
}

func (h *Hub) serveDisplay(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		h.applyDisplay(w, r)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	h.mu.Lock()
	payload := map[string]any{
		"width":       h.display.Width,
		"height":      h.display.Height,
		"fps":         h.display.FPS,
		"uiScale":     h.display.UIScale,
		"hevc":        h.display.HEVC,
		"orientation": h.display.orientation(),
		"presets":     h.display.Presets,
	}
	h.mu.Unlock()
	writeJSON(w, http.StatusOK, payload)
}

func (h *Hub) applyDisplay(w http.ResponseWriter, r *http.Request) {
	if r.URL.Query().Get("token") != accessToken {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "invalid token"})
		return
	}
	var request struct {
		Width   int  `json:"width"`
		Height  int  `json:"height"`
		FPS     int  `json:"fps"`
		UIScale int  `json:"uiScale"`
		HEVC    bool `json:"hevc"`
	}
	if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "malformed request"})
		return
	}
	h.mu.Lock()
	display := h.display
	h.mu.Unlock()

	if err := display.validate(request.Width, request.Height, request.FPS, request.HEVC); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	if err := display.apply(request.Width, request.Height, request.FPS); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	data, err := json.Marshal(payload)
	if err != nil {
		http.Error(w, "failed to encode response", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(data)
}
