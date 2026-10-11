package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

const defaultWebDir = "vendor/playport/web/dist"

// listenAddress is one address the browser client can be opened with, optionally
// annotated with the interface it belongs to.
type listenAddress struct {
	host  string
	label string
}

// viewerURLs lists the URLs the browser client can be opened with. Binding to a wildcard
// address prints one URL per interface, the way vite does, so the address that works from
// another machine is visible instead of having to be guessed. A specific bind address
// yields only that one.
func viewerURLs(addr, token string) []string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return []string{fmt.Sprintf("http://%s/?token=%s", addr, token)}
	}

	addresses := []listenAddress{{host: host}}
	if host == "" || host == "0.0.0.0" || host == "::" || host == "[::]" {
		addresses = append([]listenAddress{{host: "127.0.0.1"}}, interfaceAddresses()...)
	}

	urls := make([]string, 0, len(addresses))
	for _, entry := range addresses {
		url := fmt.Sprintf("http://%s/?token=%s", net.JoinHostPort(entry.host, port), token)
		if entry.label != "" {
			url += " (" + entry.label + ")"
		}
		urls = append(urls, url)
	}
	return urls
}

// interfaceAddresses returns the usable IPv4 addresses of every interface that is up,
// annotated with the interface name. Loopback and link-local addresses are skipped: the
// former is listed separately, the latter is never reachable.
func interfaceAddresses() []listenAddress {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var addresses []listenAddress
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			network, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			ip := network.IP.To4()
			if ip == nil || ip.IsLoopback() || ip.IsLinkLocalUnicast() {
				continue
			}
			addresses = append(addresses, listenAddress{host: ip.String(), label: iface.Name})
		}
	}
	return addresses
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8080", "HTTP listen address")
	hostSocket := flag.String("host-socket", "/tmp/carlife-host.sock", "Unix socket path of carlife-host")
	webDir := flag.String("web", defaultWebDir, "static web asset directory")
	token := flag.String("token", "carlife", "access token the browser client must present")
	deviceName := flag.String("device-name", "CarLife", "head unit name shown in the web client")
	width := flag.Int("width", 1280, "requested phone video width")
	height := flag.Int("height", 720, "requested phone video height")
	fps := flag.Int("fps", 60, "requested phone video frame rate (the phone may cap it; the accepted value is logged)")
	flag.Parse()

	log.SetFlags(log.Ltime)

	if _, err := os.Stat(*webDir); err != nil {
		log.Printf("warning: %s does not exist; run ./scripts/build.sh to build the browser client", *webDir)
	}

	var sess *session

	display := newDisplayState(*width, *height, *fps, func(width, height, fps int) error {
		return sess.changeDisplay(width, height, fps)
	})
	hub := newHub(*token, *deviceName, display)
	host := newHostClient(*hostSocket)
	sess = newSession(host, hub, *width, *height, *fps)
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
	for _, entry := range viewerURLs(*addr, *token) {
		log.Printf("  viewer URL   %s", entry)
	}
	log.Printf("  web assets   %s", *webDir)
	log.Printf("  waiting for  %s", *hostSocket)

	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("HTTP server exited: %v", err)
	}
	log.Printf("stopped")
}
