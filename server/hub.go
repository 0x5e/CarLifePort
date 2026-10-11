package main

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// keyframeWaitFallback bounds how long a viewer is held back while waiting for a
// keyframe. CarLife has no "request keyframe" command, so a viewer that joins in the
// middle of a long GOP can only wait; this keeps the wait finite.
const keyframeWaitFallback = 10 * time.Second

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
	mu      sync.Mutex
	viewers map[*viewer]struct{}
	session *session

	// Most recent codec setup per stream, replayed to a viewer that joins after the
	// stream already started. Without it such a viewer receives video frames but never
	// the decoder configuration, so nothing is ever painted.
	videoConfig map[byte][]byte
	audioConfig map[byte][]byte
}

func newHub() *Hub {
	return &Hub{
		viewers:     make(map[*viewer]struct{}),
		videoConfig: make(map[byte][]byte),
		audioConfig: make(map[byte][]byte),
	}
}

func (h *Hub) Routes(webDir string) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", h.serveWebSocket)
	mux.HandleFunc("/api/display", h.serveDisplay)
	mux.Handle("/", http.FileServer(http.Dir(webDir)))
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
	for v := range h.viewers {
		v.ready = false
		v.waitedSince = now
	}
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
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
				log.Printf("viewer %s waited %s without a keyframe, forwarding from a delta frame",
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
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// Local tool: allow the page to be opened from any origin.
		InsecureSkipVerify: true,
	})
	if err != nil {
		log.Printf("WebSocket handshake failed: %v", err)
		return
	}

	v := &viewer{
		conn:        conn,
		send:        make(chan outbound, 512),
		peer:        r.RemoteAddr,
		waitedSince: time.Now(),
	}
	defer func() {
		h.remove(v)
		log.Printf("viewer %s disconnected (%d left)", v.peer, h.count())
	}()
	defer conn.CloseNow()

	// The queue is drained by writeViewer, which has to be running before anything is
	// pushed into it.
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	go writeViewer(ctx, v)

	// Send the handshake and the current codec setup before the viewer takes part in
	// broadcasts, so that no video frame can overtake the configuration it needs.
	select {
	case v.send <- outbound{text: true, data: mustJSON(serverMessage{Type: "hello", DeviceName: "CarLife"})}:
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
	log.Printf("viewer %s connected (%d total)", v.peer, h.count())
	if h.session != nil {
		h.session.broadcastState()
	}

	for {
		_, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if h.session != nil {
			h.session.handleClientMessage(data)
		}
	}
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

/* ----------------------------------------------------------------- display settings */

type displayPreset struct {
	Width  int `json:"width"`
	Height int `json:"height"`
}

type displayInfo struct {
	Width   int             `json:"width"`
	Height  int             `json:"height"`
	FPS     int             `json:"fps"`
	Presets []displayPreset `json:"presets"`
}

// Common screen modes for CarLife head units.
var displayPresets = []displayPreset{
	{Width: 800, Height: 480},
	{Width: 1024, Height: 600},
	{Width: 1280, Height: 720},
	{Width: 1280, Height: 480},
	{Width: 1920, Height: 720},
}

func (h *Hub) serveDisplay(w http.ResponseWriter, r *http.Request) {
	sess := h.session
	if sess == nil {
		http.Error(w, "session not ready", http.StatusServiceUnavailable)
		return
	}

	if r.Method == http.MethodPost {
		var request struct {
			Width  int `json:"width"`
			Height int `json:"height"`
			FPS    int `json:"fps"`
		}
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			http.Error(w, "request body is not valid JSON", http.StatusBadRequest)
			return
		}
		if request.Width < 320 || request.Width > 3840 || request.Height < 240 || request.Height > 2160 {
			http.Error(w, "resolution out of range", http.StatusBadRequest)
			return
		}
		sess.handleDisplayChange(clientMessage{
			Width:  request.Width,
			Height: request.Height,
			FPS:    request.FPS,
		})
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"ok":true}`))
		return
	}

	sess.mu.Lock()
	info := displayInfo{
		Width:   sess.requestedWidth,
		Height:  sess.requestedHeight,
		FPS:     sess.requestedFPS,
		Presets: displayPresets,
	}
	sess.mu.Unlock()

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(info)
}
