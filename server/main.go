package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

// defaultSocketPath must match CL_DEFAULT_SOCKET in host/protocol.h: carlifed listens on
// this socket and carlife-host connects to it.
const defaultSocketPath = "/tmp/carlife-host.sock"

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
		fatalf(moduleSelf, "%s=%q is not an integer", name, value)
	}
	return parsed
}

// envBool is envInt for switches. It takes what strconv.ParseBool takes, so both "1"/"0"
// and "true"/"false" work.
func envBool(name string, fallback bool) bool {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		fatalf(moduleSelf, "%s=%q is not a boolean", name, value)
	}
	return parsed
}

// config is everything carlifed can be told to do. It comes from CL_-prefixed environment
// variables and from nothing else: there are no command-line flags, so whatever started
// carlifed (a terminal, a launch agent, another script) configures it the same way.
type config struct {
	bridgePath string
	hostPath   string
	addr       string
	token      string
	fps        int
	deviceName string
	width      int
	height     int
	tls        bool
	webDir     string
	socketPath string
	channel    string
	cuid       string
	supervise  bool
}

// executableDir returns the directory this binary was started from. os.Executable resolves
// symlinks on macOS already; EvalSymlinks is a second pass for the cases where it does not,
// so a symlink into dist/ still finds dist/.
func executableDir() string {
	executable, err := os.Executable()
	if err != nil {
		return "."
	}
	if resolved, err := filepath.EvalSymlinks(executable); err == nil {
		executable = resolved
	}
	return filepath.Dir(executable)
}

// loadConfig reads every knob, defaulting each path to a file next to this binary:
// scripts/build.sh puts the bridge, the host and the web assets in the same directory.
func loadConfig() config {
	dir := executableDir()
	return config{
		bridgePath: envString("CL_BRIDGE", filepath.Join(dir, "carlife-usb-bridge")),
		hostPath:   envString("CL_HOST", filepath.Join(dir, "carlife-host")),
		// The default is reachable from other machines on the LAN, and one viewer URL per
		// interface is printed the way vite does. 127.0.0.1:8080 keeps the page local.
		addr: envString("CL_ADDR", "0.0.0.0:8080"),
		// Access token the browser client must present.
		token: envString("CL_TOKEN", "0"),
		// Requested frame rate; the phone may cap it and reports the rate it accepted in
		// the log, so lower this if 60 is not accepted.
		fps:        envInt("CL_FPS", 60),
		deviceName: envString("CL_DEVICE_NAME", "CarLife"),
		width:      envInt("CL_WIDTH", 1280),
		height:     envInt("CL_HEIGHT", 720),
		// TLS cannot be dropped for the LAN case: the playport frontend decodes with
		// WebCodecs, which the browser only exposes in a secure context.
		tls:        envBool("CL_TLS", true),
		webDir:     envString("CL_WEB", filepath.Join(dir, "web")),
		socketPath: envString("CL_SOCKET", defaultSocketPath),
		// Vendor channel number; the phone checks it before it treats the head unit as
		// authenticated. The default is the one the open source head unit uses for the
		// after-market channel (CommonParams.VEHICLE_CHANNEL_AFTER_MARKET).
		channel: envString("CL_CHANNEL", "20000000"),
		// Head unit id. The guide requires it to be unique per head unit; the phone drops
		// the connection when it sees a repeat.
		cuid: envString("CL_CUID", "CarLifePort"),
		// Start and restart the bridge and the host as child processes. Turning this off
		// leaves carlifed as a plain HTTP/WebSocket server for a hand-started link.
		supervise: envBool("CL_SUPERVISE", true),
	}
}

const usageText = `usage: carlifed

Configuration comes from CL_-prefixed environment variables; there are no flags. Every
path defaults to a file next to this binary, which is where scripts/build.sh puts it.

  CL_ADDR          0.0.0.0:8080            HTTP listen address
  CL_TOKEN         0                       access token the browser client must present
  CL_FPS           60                      frame rate requested from the phone
  CL_TLS           true                    serve HTTPS with a self-signed certificate
  CL_WEB           <binary dir>/web        static web asset directory
  CL_DEVICE_NAME   CarLife                 head unit name shown in the web client
  CL_WIDTH         1280                    requested phone video width
  CL_HEIGHT        720                     requested phone video height
  CL_SOCKET        /tmp/carlife-host.sock  Unix socket carlife-host listens on
  CL_CHANNEL       20000000                vendor channel number reported to the phone
  CL_CUID          CarLifePort             head unit id reported to the phone
  CL_LOG_LEVEL     info                    debug, info, warn or error; debug adds the
                                           per-frame telemetry and the bridge's periodic
                                           per-channel statistics
  CL_SUPERVISE     true                    start and restart carlife-usb-bridge and
                                           carlife-host next to this binary
  CL_BRIDGE        <binary dir>/carlife-usb-bridge   the bridge to start
  CL_HOST          <binary dir>/carlife-host         the host to start

CL_WEB, CL_BRIDGE and CL_HOST are used as given, relative paths included.
`

func main() {
	if len(os.Args) > 1 {
		if os.Args[1] == "-h" || os.Args[1] == "--help" {
			fmt.Print(usageText)
			return
		}
		fmt.Fprintf(os.Stderr, "unexpected argument %q: configuration comes from CL_ environment variables, see carlifed --help\n", os.Args[1])
		os.Exit(2)
	}

	setLogLevel()
	cfg := loadConfig()

	if _, err := os.Stat(cfg.webDir); err != nil {
		warnf(moduleSelf, "%s does not exist; run ./scripts/build.sh to build the browser client", cfg.webDir)
	}

	var sess *session

	display := newDisplayState(cfg.width, cfg.height, cfg.fps, func(width, height, fps int) error {
		return sess.changeDisplay(width, height, fps)
	})
	hub := newHub(cfg.token, cfg.deviceName, display)
	host := newHostClient(cfg.socketPath)
	sess = newSession(host, hub, cfg.width, cfg.height, cfg.fps)
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

	server := &http.Server{Addr: cfg.addr, Handler: hub.Routes(cfg.webDir)}

	// Everything that can fail is prepared before the children exist: aborting after they
	// are running would leave them behind, still holding the USB device.
	scheme := "http"
	var certFile, keyFile string
	if cfg.tls {
		var err error
		certFile, keyFile, err = selfSignedCertificate(certificateDir())
		if err != nil {
			fatalf(moduleSelf, "cannot prepare the TLS certificate: %v", err)
		}
		scheme = "https"
		// Handshake failures go through ErrorLog. The standard logger would stamp its own
		// timestamp; the writer puts the line through logLine instead so it keeps the
		// level and module columns.
		server.ErrorLog = log.New(certificateAlertWriter{}, "", 0)
	}

	// Reported before the link is started, so that the addresses are on screen even when
	// the first connection to the phone fails.
	infof(moduleSelf, "service started")
	for _, entry := range viewerURLs(cfg.addr, cfg.token, scheme) {
		infof(moduleSelf, "viewer URL   %s", entry)
	}
	if cfg.tls {
		infof(moduleSelf, "certificate  %s (self-signed: accept the browser warning once)", certFile)
	}
	infof(moduleSelf, "web assets   %s", cfg.webDir)
	infof(moduleSelf, "socket       %s", cfg.socketPath)
	if cfg.supervise {
		infof(moduleSelf, "starting     %s", cfg.bridgePath)
		infof(moduleSelf, "             %s", cfg.hostPath)
	} else {
		infof(moduleSelf, "supervising  off (CL_SUPERVISE=false): the link has to be started by hand")
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// The supervisor owns the two children, and this process must not exit before it has
	// reaped them: exiting first orphans them, and an orphaned bridge keeps the USB device
	// claimed. With CL_SUPERVISE=false there are no children and the wait returns at once.
	supervisorDone := make(chan struct{})
	if cfg.supervise {
		if err := checkSiblings(cfg); err != nil {
			fatalf(moduleSelf, "%v", err)
		}
		go func() {
			defer close(supervisorDone)
			newSupervisor(cfg).run(ctx)
		}()
	} else {
		close(supervisorDone)
	}
	waitChildren := func() { <-supervisorDone }

	go host.run(ctx)
	go sess.sessionWatchdog(ctx)

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	var serveErr error
	if cfg.tls {
		serveErr = server.ListenAndServeTLS(certFile, keyFile)
	} else {
		serveErr = server.ListenAndServe()
	}
	if serveErr != nil && serveErr != http.ErrServerClosed {
		stop()
		waitChildren()
		fatalf(moduleSelf, "HTTP server exited: %v", serveErr)
	}
	waitChildren()
	infof(moduleSelf, "stopped")
}
