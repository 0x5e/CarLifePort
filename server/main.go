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
	"strconv"
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
func viewerURLs(addr, token, scheme string) []string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return []string{fmt.Sprintf("%s://%s/?token=%s", scheme, addr, token)}
	}

	addresses := []listenAddress{{host: host}}
	if host == "" || host == "0.0.0.0" || host == "::" || host == "[::]" {
		addresses = append([]listenAddress{{host: "127.0.0.1"}}, interfaceAddresses()...)
	}

	urls := make([]string, 0, len(addresses))
	for _, entry := range addresses {
		url := fmt.Sprintf("%s://%s/?token=%s", scheme, net.JoinHostPort(entry.host, port), token)
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

// envString returns the value of the environment variable name, or fallback when it is
// unset or empty.
func envString(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

// envInt returns the value of the environment variable name parsed as an integer, or
// fallback when it is unset or empty. A value that is present but not a number aborts:
// silently falling back would turn a typo into an unnoticed wrong frame rate.
func envInt(name string, fallback int) int {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil {
		log.Fatalf("%s=%q is not an integer", name, value)
	}
	return parsed
}

func main() {
	// These three are read from the environment instead of the command line, so that
	// run.sh does not have to restate their defaults and a bare carlifed run behaves
	// the same as the scripted one.
	// HTTP listen address: the default is reachable from other machines on the LAN, and
	// one viewer URL per interface is printed the way vite does. 127.0.0.1:8080 keeps
	// the page on this machine only.
	addr := envString("ADDR", "0.0.0.0:8080")
	// Access token the browser client must present.
	token := envString("TOKEN", "0")
	// Requested frame rate; the phone may cap it and reports the rate it accepted in the
	// log, so lower this if 60 is not accepted.
	fps := envInt("FPS", 60)

	hostSocket := flag.String("host-socket", "/tmp/carlife-host.sock", "Unix socket path of carlife-host")
	webDir := flag.String("web", defaultWebDir, "static web asset directory")
	deviceName := flag.String("device-name", "CarLife", "head unit name shown in the web client")
	width := flag.Int("width", 1280, "requested phone video width")
	height := flag.Int("height", 720, "requested phone video height")
	useTLS := flag.Bool("tls", true, "serve HTTPS with a self-signed certificate; plain http only works on localhost because the browser client needs a secure context")
	flag.Parse()

	log.SetFlags(log.Ltime)

	if _, err := os.Stat(*webDir); err != nil {
		log.Printf("warning: %s does not exist; run ./scripts/build.sh to build the browser client", *webDir)
	}

	var sess *session

	display := newDisplayState(*width, *height, fps, func(width, height, fps int) error {
		return sess.changeDisplay(width, height, fps)
	})
	hub := newHub(token, *deviceName, display)
	host := newHostClient(*hostSocket)
	sess = newSession(host, hub, *width, *height, fps)
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

	server := &http.Server{Addr: addr, Handler: hub.Routes(*webDir)}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	scheme := "http"
	var certFile, keyFile string
	if *useTLS {
		var err error
		certFile, keyFile, err = selfSignedCertificate(certificateDir())
		if err != nil {
			log.Fatalf("cannot prepare the TLS certificate: %v", err)
		}
		scheme = "https"
		// Handshake failures go through ErrorLog; the writer only drops the alerts a
		// self-signed certificate provokes. The no-prefix logger keeps the default format.
		server.ErrorLog = log.New(certificateAlertWriter{}, "", 0)
	}

	log.Printf("CarLife head unit service started")
	for _, entry := range viewerURLs(addr, token, scheme) {
		log.Printf("  viewer URL   %s", entry)
	}
	if *useTLS {
		log.Printf("  certificate  %s (self-signed: accept the browser warning once)", certFile)
	}
	log.Printf("  web assets   %s", *webDir)
	log.Printf("  waiting for  %s", *hostSocket)

	var err error
	if *useTLS {
		err = server.ListenAndServeTLS(certFile, keyFile)
	} else {
		err = server.ListenAndServe()
	}
	if err != nil && err != http.ErrServerClosed {
		log.Fatalf("HTTP server exited: %v", err)
	}
	log.Printf("stopped")
}
