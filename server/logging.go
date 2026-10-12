package main

import (
	"fmt"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"
)

// Log levels, lowest first. A message is written when its level is at least the level
// selected by CL_LOG_LEVEL, so the default (info) keeps state changes and drops the
// periodic telemetry that would otherwise dominate a long-running session.
type logLevel int

const (
	levelDebug logLevel = iota
	levelInfo
	levelWarn
	levelError
)

// levelNames is indexed by logLevel.
var levelNames = [...]string{"DEBUG", "INFO", "WARN", "ERROR"}

// Log modules: one per subsystem, so a line can be traced to the file it came from
// without reading the message. moduleBridge and moduleHost are the output of the two
// child processes, relayed by this one.
const (
	moduleSelf       = "carlifed"   // main.go: startup, shutdown, configuration
	moduleSupervisor = "supervisor" // supervisor.go: the two child processes
	moduleLink       = "link"       // hostclient.go: the Unix socket to carlife-host
	moduleSession    = "session"    // session.go: the CarLife session
	moduleViewer     = "viewer"     // hub.go: browser clients and the HTTP server
	moduleBridge     = "bridge"     // relayed output of carlife-usb-bridge
	moduleHost       = "host"       // relayed output of carlife-host
)

// moduleWidth is the module column width (the longest module name). Everything after it
// starts at the same column.
const moduleWidth = len(moduleSupervisor)

var (
	logMu   sync.Mutex
	current = levelInfo
)

// setLogLevel reads CL_LOG_LEVEL. An unknown value aborts instead of falling back, for
// the same reason envInt does: silently ignoring a typo would select a level nobody
// asked for. "verbose" is accepted as another spelling of debug.
func setLogLevel() {
	value := strings.ToLower(strings.TrimSpace(os.Getenv("CL_LOG_LEVEL")))
	switch value {
	case "":
		return
	case "debug", "verbose":
		current = levelDebug
	case "info":
		current = levelInfo
	case "warn", "warning":
		current = levelWarn
	case "error":
		current = levelError
	default:
		fmt.Fprintf(os.Stderr, "CL_LOG_LEVEL=%q is not one of debug, info, warn, error\n", value)
		os.Exit(2)
	}
}

// logLine writes "time level module message" to stderr.
//
// stderr rather than stdout because the child processes write there too and the three
// streams end up in one terminal; keeping this one on stderr as well means the order is
// preserved when the two are redirected together.
func logLine(level logLevel, module, format string, args ...any) {
	if level < current {
		return
	}
	message := fmt.Sprintf(format, args...)
	timestamp := time.Now().Format("15:04:05.000")
	logMu.Lock()
	defer logMu.Unlock()
	if module == "" {
		fmt.Fprintf(os.Stderr, "%s %-5s %s\n", timestamp, levelNames[level], message)
		return
	}
	fmt.Fprintf(os.Stderr, "%s %-5s %-*s %s\n", timestamp, levelNames[level], moduleWidth, module, message)
}

func debugf(module, format string, args ...any) { logLine(levelDebug, module, format, args...) }
func infof(module, format string, args ...any)  { logLine(levelInfo, module, format, args...) }
func warnf(module, format string, args ...any)  { logLine(levelWarn, module, format, args...) }
func errorf(module, format string, args ...any) { logLine(levelError, module, format, args...) }

// fatalf reports and exits. It is used for the conditions that leave nothing worth
// running: bad configuration, a missing child, a port that cannot be bound.
func fatalf(module, format string, args ...any) {
	logLine(levelError, module, format, args...)
	os.Exit(1)
}

// The children stamp every line they write ("[17:01:24] ..." from the bridge,
// "[17:01:24.123] ..." from the host), and a level marker after it where the line is not
// info ("[17:01:24] DEBUG ..."). Both are dropped here: this process adds its own
// timestamp and puts the level in its own column.
var (
	childTimestamp = regexp.MustCompile(`^\[\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?\]\s*`)
	childLevel     = regexp.MustCompile(`^(DEBUG|INFO|WARN|WARNING|ERROR)\s+`)
)

// parseChildLine splits one relayed line into its level and its message. A line with no
// marker is info, which is what all the children's one-off diagnostics are.
func parseChildLine(line string) (logLevel, string) {
	line = strings.TrimRight(line, "\r\n")
	line = childTimestamp.ReplaceAllString(line, "")
	level := levelInfo
	if match := childLevel.FindStringSubmatch(line); match != nil {
		level = levelFromName(match[1])
		line = line[len(match[0]):]
	}
	return level, line
}

// levelFromName maps a marker to a level. Every name the regexp accepts is known here.
func levelFromName(name string) logLevel {
	switch name {
	case "DEBUG":
		return levelDebug
	case "WARN", "WARNING":
		return levelWarn
	case "ERROR":
		return levelError
	}
	return levelInfo
}
