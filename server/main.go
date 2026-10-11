package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8080", "HTTP listen address")
	hostSocket := flag.String("host-socket", "/tmp/carlife-host.sock", "Unix socket path of carlife-host")
	webDir := flag.String("web", "web/dist", "static web asset directory")
	width := flag.Int("width", 1280, "requested phone video width")
	height := flag.Int("height", 720, "requested phone video height")
	fps := flag.Int("fps", 30, "requested phone video frame rate")
	flag.Parse()

	log.SetFlags(log.Ltime)

	if _, err := os.Stat(*webDir); err != nil {
		log.Printf("warning: %s does not exist; run npm install && npm run build in web/ first", *webDir)
	}

	hub := newHub()
	host := newHostClient(*hostSocket)
	sess := newSession(host, hub, *width, *height, *fps)
	hub.session = sess
	host.onEvent = sess.onHostEvent
	host.onState = func(connected bool) {
		// A host reconnect means the previous CarLife session is gone: carlife-host is
		// restarted together with the bridge, so every stream state carried over from
		// the old link must be dropped.
		//
		// hostReady is deliberately not set here. The socket being up only means the host
		// process exists; the host accepts this connection and reports the real channel
		// state in its EV_STATUS frame, which is sent only after connectionSetup()
		// succeeded. Marking it ready here would make the watchdog send a protocol
		// version while the six channels are still down.
		sess.resetStream()
		if !connected {
			sess.mu.Lock()
			sess.hostReady = false
			sess.mu.Unlock()
		}
		sess.broadcastState()
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go host.run(ctx)
	go sess.sessionWatchdog(ctx)

	server := &http.Server{Addr: *addr, Handler: hub.Routes(*webDir)}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	log.Printf("CarLife head unit service started")
	log.Printf("  open browser  http://%s", *addr)
	log.Printf("  web assets    %s", *webDir)
	log.Printf("  waiting for   %s", *hostSocket)

	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("HTTP server exited: %v", err)
	}
	log.Printf("stopped")
}
