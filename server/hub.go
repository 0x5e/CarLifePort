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

type outbound struct {
	text bool
	data []byte
}

type viewer struct {
	conn *websocket.Conn
	send chan outbound
}

// Hub 管理所有浏览器连接，并向上游 session 转发它们的控制消息。
type Hub struct {
	mu      sync.Mutex
	viewers map[*viewer]struct{}
	session *session
}

func newHub() *Hub {
	return &Hub{viewers: make(map[*viewer]struct{})}
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

// Broadcast 把一帧二进制数据发给所有浏览器；消费不过来的连接直接丢弃该帧。
func (h *Hub) Broadcast(data []byte) {
	if len(data) == 0 {
		return
	}
	h.mu.Lock()
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
		viewers = append(viewers, v)
	}
	h.mu.Unlock()
	for _, v := range viewers {
		select {
		case v.send <- outbound{data: data}:
		default:
		}
	}
}

// BroadcastJSON 把一条控制消息发给所有浏览器。
func (h *Hub) BroadcastJSON(value any) {
	data, err := json.Marshal(value)
	if err != nil {
		return
	}
	h.mu.Lock()
	viewers := make([]*viewer, 0, len(h.viewers))
	for v := range h.viewers {
		viewers = append(viewers, v)
	}
	h.mu.Unlock()
	for _, v := range viewers {
		select {
		case v.send <- outbound{text: true, data: data}:
		default:
		}
	}
}

func (h *Hub) serveWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// 本地工具，允许任意来源打开页面。
		InsecureSkipVerify: true,
	})
	if err != nil {
		log.Printf("WebSocket 握手失败：%v", err)
		return
	}

	v := &viewer{conn: conn, send: make(chan outbound, 512)}
	h.add(v)
	defer h.remove(v)

	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	defer conn.CloseNow()

	go writeViewer(ctx, v)

	// 先把当前状态补给新连接，再让 session 广播一次完整状态。
	select {
	case v.send <- outbound{text: true, data: mustJSON(serverMessage{Type: "hello", DeviceName: "CarLife"})}:
	default:
	}
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

/* ----------------------------------------------------------------- 画面设置 */

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

// CarLife 车机端常见的几种屏幕规格。
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
		http.Error(w, "会话未就绪", http.StatusServiceUnavailable)
		return
	}

	if r.Method == http.MethodPost {
		var request struct {
			Width  int `json:"width"`
			Height int `json:"height"`
			FPS    int `json:"fps"`
		}
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			http.Error(w, "请求体不是合法的 JSON", http.StatusBadRequest)
			return
		}
		if request.Width < 320 || request.Width > 3840 || request.Height < 240 || request.Height > 2160 {
			http.Error(w, "分辨率超出可用范围", http.StatusBadRequest)
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
