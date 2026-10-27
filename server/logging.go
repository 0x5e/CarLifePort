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
// without reading the message. moduleBridge and moduleStack are the two halves of the link
// process, whose output is relayed by this one.
const (
	moduleSelf       = "carlifed"   // main.go: startup, shutdown, configuration
	moduleSupervisor = "supervisor" // supervisor.go: the link process
	moduleLink       = "link"       // linkclient.go: the Unix socket to carlife-link
	moduleSession    = "session"    // session.go: the CarLife session
	moduleViewer     = "viewer"     // hub.go: browser clients and the HTTP server
	moduleBridge     = "bridge"     // relayed output of the link's USB transport
	moduleStack      = "stack"      // relayed output of the link's protocol stack
)

// moduleWidth is the module column width: the longest module name plus the brackets it is
// printed in. Everything after it starts at the same column.
const moduleWidth = len(moduleSupervisor) + 2

var (
	logMu   sync.Mutex
	current = levelInfo
)

// setLogLevel reads CL_LOG_LEVEL. An unknown value aborts instead of falling back, for
// the same reason envInt does: silently ignoring a typo would select a level nobody
// asked for.
func setLogLevel() {
	value := strings.ToLower(strings.TrimSpace(os.Getenv("CL_LOG_LEVEL")))
	switch value {
	case "":
		return
	case "debug":
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

// ANSI colours: the timestamp is greyed out, the level column takes the colour of its
// severity, and the module and the message keep the terminal's default colour.
const (
	colourReset  = "\033[0m"
	colourGrey   = "\033[90m"
	colourWhite  = "\033[97m"
	colourGreen  = "\033[32m"
	colourYellow = "\033[33m"
	colourRed    = "\033[31m"
)

// levelColours is indexed by logLevel: debug white, info green, warn yellow, error red.
var levelColours = [...]string{colourWhite, colourGreen, colourYellow, colourRed}

// colourEnabled is decided once at startup: colours are for a terminal, and emitting the
// escape sequences into a redirected stderr would only put them in the file.
var colourEnabled = func() bool {
	info, err := os.Stderr.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0
}()

// logLine writes "time level [module] <event> <key>=<value> ..." to stderr; the module column
// is omitted for the few lines that belong to no subsystem.
//
// The event is a stable, lower-case, dotted name of the form <area>.<subject>_<outcome>
// (session.channels_ready, video.encoder_start_failed): it is what a search keys on, so it
// outlives the fields around it. Fields carry the numbers, unit in the key (retry_ms,
// rate_hz) and never in the value. The level says how bad it is, the event what happened;
// prose belongs in neither.
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
	name := fmt.Sprintf("%-5s", levelNames[level])
	column := ""
	if module != "" {
		column = fmt.Sprintf(" %-*s", moduleWidth, "["+module+"]")
	}
	logMu.Lock()
	defer logMu.Unlock()
	if !colourEnabled {
		fmt.Fprintf(os.Stderr, "%s %s%s %s\n", timestamp, name, column, message)
		return
	}
	fmt.Fprintf(os.Stderr, "%s%s%s %s%s%s%s %s\n",
		colourGrey, timestamp, colourReset,
		levelColours[level], name, colourReset,
		column, message)
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

// The link process stamps every line it writes ("[17:01:24] ..." from the USB transport,
// "[17:01:24.123] ..." from the protocol stack), a level marker after it where the line is
// not info ("[17:01:24] DEBUG ..."), and the name of the half it came from ("bridge" or
// "stack"). All three are read back here: this process adds its own timestamp, puts the
// level in its own column and turns the half into the module column.
var (
	childTimestamp = regexp.MustCompile(`^\[\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?\]\s*`)
	childLevel     = regexp.MustCompile(`^(DEBUG|INFO|WARN|WARNING|ERROR)\s+`)
	childModule    = regexp.MustCompile(`^(bridge|stack)\s+`)
)

// childModules maps the token the link writes to the module column it is logged under.
var childModules = map[string]string{
	"bridge": moduleBridge,
	"stack":  moduleStack,
}

// parseChildLine splits one relayed line into its level, its module and its message. The
// module is empty when the line carries no token, which is the case for anything the child
// writes without going through its own logging functions; the caller falls back to the
// module it started the child under. A line with no level marker is info, which is what all
// the one-off diagnostics are.
func parseChildLine(line string) (logLevel, string, string) {
	line = strings.TrimRight(line, "\r\n")
	line = childTimestamp.ReplaceAllString(line, "")
	level := levelInfo
	if match := childLevel.FindStringSubmatch(line); match != nil {
		level = levelFromName(match[1])
		line = line[len(match[0]):]
	}
	module := ""
	if match := childModule.FindStringSubmatch(line); match != nil {
		module = childModules[match[1]]
		line = line[len(match[0]):]
	}
	return level, module, line
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
