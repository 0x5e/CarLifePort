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
		sess.mu.Lock()
		sess.hostReady = connected
		sess.mu.Unlock()
		sess.broadcastState()
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go host.run(ctx)

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
