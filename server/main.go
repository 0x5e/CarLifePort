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
	"strings"
	"syscall"
	"time"
)

// defaultSocketPath is where carlife-link listens and carlifed connects. The value lives
// here alone: carlife-link has no default of its own and is told the path with --socket.
const defaultSocketPath = "/tmp/carlife-link.sock"

// listenAddress is one address the browser client can be opened with, optionally
// annotated with the interface it belongs to.
type listenAddress struct {
	host  string
	label string
}

// viewerURLs lists the URLs the browser client can be opened with. Binding to a wildcard
// address prints one URL per interface, so the address that works from another machine is
// visible instead of having to be guessed. A specific bind address yields only that one.
func viewerURLs(addr, token, scheme string) []string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return []string{fmt.Sprintf("%s://%s/?token=%s", scheme, addr, token)}
	}

	addresses := []listenAddress{{host: host}}
	if host == "" || host == "0.0.0.0" || host == "::" || host == "[::]" {
		addresses = interfaceAddresses()
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
// annotated with the interface name. Link-local addresses are skipped, never reachable
// from another machine. The loopback interface is not treated specially, so its URL
// carries a name like every other one. Docker and VM bridges are skipped: their addresses
// belong to a network inside this machine that nothing else can route to.
func interfaceAddresses() []listenAddress {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var addresses []listenAddress
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || strings.HasPrefix(iface.Name, "bridge") {
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
			if ip == nil || ip.IsLinkLocalUnicast() {
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

// config is everything carlifed can be told to do. It comes from CL_-prefixed environment
// variables and from nothing else: there are no command-line flags, so whatever started
// carlifed (a terminal, a launch agent, another script) configures it the same way.
type config struct {
	linkPath   string
	addr       string
	token      string
	fps        int
	deviceName string
	width      int
	height     int
	webDir     string
	channel    string
	cuid       string
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
// the top-level Makefile puts the link program and the web assets in the same directory.
func loadConfig() config {
	dir := executableDir()
	return config{
		linkPath: envString("CL_LINK", filepath.Join(dir, "carlife-link")),
		// The default is reachable from other machines on the LAN; one viewer URL is
		// printed per interface. 127.0.0.1:8080 keeps the page local.
		addr: envString("CL_ADDR", "0.0.0.0:8080"),
		// Access token the browser client must present.
		token: envString("CL_TOKEN", "0"),
		// Requested frame rate; the phone may cap it and reports the rate it accepted in
		// the log, so lower this if 60 is not accepted.
		fps:        envInt("CL_FPS", 60),
		deviceName: envString("CL_DEVICE_NAME", "CarLife"),
		width:      envInt("CL_WIDTH", 1280),
		height:     envInt("CL_HEIGHT", 720),
		webDir:     envString("CL_WEB", filepath.Join(dir, "web")),
		// Vendor channel number; the phone checks it before it treats the head unit as
		// authenticated. The default is the one the open source head unit uses for the
		// after-market channel (CommonParams.VEHICLE_CHANNEL_AFTER_MARKET).
		channel: envString("CL_CHANNEL", "20000000"),
		// Head unit id. The guide requires it to be unique per head unit; the phone drops
		// the connection when it sees a repeat.
		cuid: envString("CL_CUID", "CarLifePort"),
	}
}

const usageText = `usage: carlifed

Configuration comes from CL_-prefixed environment variables; there are no flags. Every
path defaults to a file next to this binary, which is where the Makefile puts it.

  CL_ADDR          0.0.0.0:8080            HTTP listen address
  CL_TOKEN         0                       access token the browser client must present
  CL_FPS           60                      frame rate requested from the phone
  CL_WEB           <binary dir>/web        static web asset directory
  CL_DEVICE_NAME   CarLife                 head unit name shown in the web client
  CL_WIDTH         1280                    requested phone video width
  CL_HEIGHT        720                     requested phone video height
  CL_CHANNEL       20000000                vendor channel number reported to the phone
  CL_CUID          CarLifePort             head unit id reported to the phone
  CL_LOG_LEVEL     info                    debug, info, warn or error; debug adds the
                                           per-frame telemetry, the link's periodic
                                           per-channel statistics, and the link's copy of
                                           the events this process already reports
  CL_LINK          <binary dir>/carlife-link         the link to start

CL_WEB and CL_LINK are used as given, relative paths included.
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
		warnf(moduleSelf, "%s does not exist; run make to build the browser client", cfg.webDir)
	}

	var sess *session

	display := newDisplayState(cfg.width, cfg.height, cfg.fps, func(width, height, fps int) error {
		return sess.changeDisplay(width, height, fps)
	})
	hub := newHub(cfg.token, cfg.deviceName, display)
	link := newLinkClient(defaultSocketPath)
	sess = newSession(link, hub, cfg.width, cfg.height, cfg.fps)
	hub.session = sess

	link.onEvent = sess.onLinkEvent
	link.onState = func(connected bool) {
		// A link reconnect means the previous CarLife session is gone: the link process is
		// restarted as a whole, so every stream state carried over from the old link has
		// to be dropped.
		//
		// linkReady is deliberately not set here. The socket being up only means the link
		// process exists; it accepts this connection and reports the real channel state in
		// its EV_STATUS frame, which is sent only after connectionSetup() succeeded.
		// Marking it ready here would make the watchdog send a protocol version while the
		// six channels are still down.
		sess.resetStream()
		if !connected {
			sess.mu.Lock()
			sess.linkReady = false
			sess.mu.Unlock()
		}
		sess.broadcastState()
	}

	server := &http.Server{Addr: cfg.addr, Handler: hub.Routes(cfg.webDir)}

	// Everything that can fail is prepared before the children exist: aborting after they
	// are running would leave them behind, still holding the USB device.
	//
	// TLS is not optional: the playport frontend decodes with WebCodecs, which the
	// browser only exposes in a secure context, so plain http would work on localhost
	// alone.
	certFile, keyFile, err := selfSignedCertificate(certificateDir())
	if err != nil {
		fatalf(moduleSelf, "cannot prepare the TLS certificate: %v", err)
	}
	// Handshake failures go through ErrorLog. The standard logger would stamp its own
	// timestamp; the writer puts the line through logLine instead so it keeps the level
	// and module columns.
	server.ErrorLog = log.New(certificateAlertWriter{}, "", 0)

	// Reported before the link is started, so that the addresses are on screen even when
	// the first connection to the phone fails.
	infof(moduleSelf, "service started")
	for _, entry := range viewerURLs(cfg.addr, cfg.token, "https") {
		infof(moduleSelf, "viewer URL   %s", entry)
	}
	infof(moduleSelf, "web assets   %s", cfg.webDir)
	infof(moduleSelf, "socket       %s", defaultSocketPath)
	infof(moduleSelf, "starting     %s", cfg.linkPath)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// The supervisor owns the link process, and this process must not exit before it has
	// reaped it: exiting first orphans it, and an orphaned link keeps the USB device
	// claimed.
	if err := checkSibling(cfg); err != nil {
		fatalf(moduleSelf, "%v", err)
	}
	supervisorDone := make(chan struct{})
	go func() {
		defer close(supervisorDone)
		newSupervisor(cfg).run(ctx)
	}()

	go link.run(ctx)
	go sess.sessionWatchdog(ctx)

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	serveErr := server.ListenAndServeTLS(certFile, keyFile)
	if serveErr != nil && serveErr != http.ErrServerClosed {
		stop()
		<-supervisorDone
		fatalf(moduleSelf, "HTTP server exited: %v", serveErr)
	}
	<-supervisorDone
	infof(moduleSelf, "stopped")
}
