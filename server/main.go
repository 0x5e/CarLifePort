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

// accessToken is the value the browser client presents on /ws and /api/display, and the
// one printed in the viewer URL. It is a constant rather than a knob: the client reads it
// from the URL once, keeps it in localStorage and replays it from there, so a per-machine
// value would only be something else to carry around. It is not a security boundary --
// this is a service for the machine it runs on.
const accessToken = "0"

// listenAddress is one address the browser client can be opened with, optionally
// annotated with the interface it belongs to.
type listenAddress struct {
	host  string
	label string
}

// viewerURLs lists the URLs the browser client can be opened with. Binding to a wildcard
// address prints one URL per interface, so the address that works from another machine is
// visible instead of having to be guessed. A specific bind address yields only that one.
func viewerURLs(addr, scheme string) []string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return []string{fmt.Sprintf("%s://%s/?token=%s", scheme, addr, accessToken)}
	}

	addresses := []listenAddress{{host: host}}
	if host == "" || host == "0.0.0.0" || host == "::" || host == "[::]" {
		addresses = interfaceAddresses()
	}

	urls := make([]string, 0, len(addresses))
	for _, entry := range addresses {
		url := fmt.Sprintf("%s://%s/?token=%s", scheme, net.JoinHostPort(entry.host, port), accessToken)
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

// hostName returns the machine's hostname, the head unit name the web client shows. The
// fallback only matters if the lookup fails, which it does not in practice; it keeps the
// browser from showing an empty name.
func hostName() string {
	name, err := os.Hostname()
	if err != nil || name == "" {
		return "CarLife"
	}
	return name
}

// machineID returns this machine's own identifier, reported to the phone as the head unit
// id. The guide requires the value to be unique per head unit (the phone drops the
// connection when it sees a repeat), which is why it is read from the machine instead of
// being a constant.
func machineID() string {
	if id := hostUUID(); id != "" {
		return id
	}
	// No identifier from the platform (a stripped down system, or a lookup that failed):
	// the hostname is what is left, even though two machines can share one.
	warnf(moduleSelf, "no host uuid available, reporting the hostname as the head unit id")
	return hostName()
}

// parseVideoMode reads the "<width>x<height>@<fps>" form of CL_VIDEO_MODE, which is how a
// video mode is normally written (1920x720@60). A value that does not parse aborts: asking
// the phone to encode at a resolution nobody chose is worse than not starting.
func parseVideoMode(value string) (int, int, int) {
	size, rate, hasRate := strings.Cut(value, "@")
	width, height, hasSize := strings.Cut(size, "x")
	widthPixels, widthErr := strconv.Atoi(width)
	heightPixels, heightErr := strconv.Atoi(height)
	fps, fpsErr := strconv.Atoi(rate)
	if !hasRate || !hasSize || widthErr != nil || heightErr != nil || fpsErr != nil {
		fatalf(moduleSelf, "CL_VIDEO_MODE=%q is not <width>x<height>@<fps>, for example 1920x720@60", value)
	}
	return widthPixels, heightPixels, fps
}

// config is everything carlifed can be told to do. It comes from CL_-prefixed environment
// variables and from nothing else: there are no command-line flags, so whatever started
// carlifed (a terminal, a launch agent, another script) configures it the same way.
type config struct {
	linkPath   string
	addr       string
	fps        int
	deviceName string
	width      int
	height     int
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

// loadConfig reads every knob, defaulting each path to a file next to this binary: the
// top-level Makefile puts the link program there. The browser client is not a path at all:
// it is compiled into this binary.
func loadConfig() config {
	dir := executableDir()
	// The video mode is one knob: the phone is told width, height and frame rate in a single
	// message, so the three are only meaningful together. The phone may cap the frame rate
	// and reports the rate it accepted in the log, so lower this if 60 is not accepted. The
	// web client's display panel changes it at runtime.
	width, height, fps := parseVideoMode(envString("CL_VIDEO_MODE", "1280x720@60"))
	return config{
		linkPath: envString("CL_LINK", filepath.Join(dir, "carlife-link")),
		// The default is reachable from other machines on the LAN; one viewer URL is
		// printed per interface. 127.0.0.1:8080 keeps the page local.
		addr: envString("CL_WEB_ADDR", "0.0.0.0:8080"),
		// The head unit name the web client shows. It is this machine's hostname rather
		// than a knob: on the target device the hostname is the name the user gave it, and
		// a name that matches the machine is easier to recognize than a fixed one.
		deviceName: hostName(),
		width:      width,
		height:     height,
		fps:        fps,
		// Vendor channel number; the phone checks it before it treats the head unit as
		// authenticated. The check the CarLife app applies is: the trimmed string is
		// non-empty, matches 2 followed by exactly seven digits, and is neither 20000000
		// (after-market) nor 20000001 (pre-install). Everything else passes, and the value
		// then selects that vendor's feature set, so the default is the Mercedes-Benz one
		// (20062100, VehicleChannel.VEHICLE_CHANNEL_DAIMLER in the app). Benz is also the
		// only channel for which the app additionally requires itself to be the official
		// build (SignatureVerification.packageSignatureCheck) -- the genuine app passes, and
		// a repackaged one is already turned away by the check the app runs on the
		// statistics info itself, whatever the channel is. See README "Head unit
		// authentication".
		channel: envString("CL_CHANNEL", "20062100"),
		// Head unit id, taken from the machine: the guide requires it to be unique per head
		// unit, and the phone drops the connection when it sees a repeat.
		cuid: machineID(),
	}
}

const usageText = `usage: carlifed

Configuration comes from CL_-prefixed environment variables; there are no flags. CL_LINK
defaults to a file next to this binary, which is where the Makefile puts it.

  CL_VIDEO_MODE    1280x720@60             video mode requested from the phone, as
                                           <width>x<height>@<fps>
  CL_WEB_ADDR      0.0.0.0:8080            HTTPS listen address
  CL_CHANNEL       20062100                vendor channel number reported to the phone
  CL_LOG_LEVEL     info                    debug, info, warn or error; debug adds the
                                           per-frame telemetry and the link's copy of
                                           the events this process already reports
  CL_LINK          <binary dir>/carlife-link         the link to start

CL_LINK is used as given, relative paths included.
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

	var sess *session

	display := newDisplayState(cfg.width, cfg.height, cfg.fps, func(width, height, fps int) error {
		return sess.changeDisplay(width, height, fps)
	})
	hub := newHub(cfg.deviceName, display)
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

	server := &http.Server{Addr: cfg.addr, Handler: hub.Routes()}

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
	for _, entry := range viewerURLs(cfg.addr, "https") {
		infof(moduleSelf, "viewer URL   %s", entry)
	}
	infof(moduleSelf, "web assets   embedded")
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
