package main

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"time"
)

// restartDelay is how long to wait before rebuilding the link after the bridge or the host
// exits. Reseating the cable needs a moment: the phone re-enumerates and only then does the
// system start the CarLife app again.
const restartDelay = 2 * time.Second

// stopTimeout bounds the wait for a child to exit after SIGTERM before it is killed.
const stopTimeout = 3 * time.Second

// checkSiblings verifies that the two processes this binary starts are where it looks for
// them, before anything is sent to the phone.
func checkSiblings(cfg config) error {
	for _, path := range []string{cfg.bridgePath, cfg.hostPath} {
		info, err := os.Stat(path)
		if err != nil {
			return fmt.Errorf("missing %s, run ./scripts/build.sh first", path)
		}
		if info.Mode().Perm()&0o111 == 0 {
			return fmt.Errorf("%s is not executable, run ./scripts/build.sh first", path)
		}
	}
	return nil
}

// supervisor starts carlife-usb-bridge and carlife-host next to this binary and restarts
// them together whenever either one exits, so that starting a link is one process to start
// and one to stop.
//
// The two are restarted as a pair because the link cannot be resumed in place:
// CarLifeVehicleLib only establishes its six channels inside connectionSetup(), and the
// phone side starts a fresh session on every accessory attach.
type supervisor struct {
	bridgePath string
	hostPath   string
	hostArgs   []string
}

func newSupervisor(cfg config) *supervisor {
	return &supervisor{
		bridgePath: cfg.bridgePath,
		hostPath:   cfg.hostPath,
		// The host reads its flags from the command line; carlifed owns the values, so it
		// passes them explicitly instead of relying on the host's own defaults.
		hostArgs: []string{
			"--socket", cfg.socketPath,
			"--channel", cfg.channel,
			"--cuid", cfg.cuid,
		},
	}
}

func (s *supervisor) run(ctx context.Context) {
	for {
		bridge, host := s.startPair()

		if bridge == nil && host == nil {
			// Neither one is running, so there is nothing to wait for: back off and retry.
			if !waitOrStop(ctx, restartDelay) {
				return
			}
			continue
		}

		select {
		case <-ctx.Done():
		case err := <-exited(bridge):
			infof(moduleSupervisor, "carlife-usb-bridge exited (%s), rebuilding the link", exitReason(err))
		case err := <-exited(host):
			infof(moduleSupervisor, "carlife-host exited (%s), rebuilding the link", exitReason(err))
		}

		bridge.stop()
		host.stop()
		if ctx.Err() != nil {
			return
		}
		infof(moduleSupervisor, "rebuilding the link in %s (Ctrl-C to stop)", restartDelay)
		if !waitOrStop(ctx, restartDelay) {
			return
		}
	}
}

// startPair runs both children. A child that cannot be started is reported and skipped: the
// other one is still worth running, and the retry loop will try again.
func (s *supervisor) startPair() (bridge, host *childProcess) {
	bridge, err := startChild(moduleBridge, s.bridgePath)
	if err != nil {
		errorf(moduleSupervisor, "cannot start %s: %v", s.bridgePath, err)
	}
	host, err = startChild(moduleHost, s.hostPath, s.hostArgs...)
	if err != nil {
		errorf(moduleSupervisor, "cannot start %s: %v", s.hostPath, err)
	}
	return bridge, host
}

// childProcess is a running child plus the channel that reports how it ended.
type childProcess struct {
	// module is the name the child's output is tagged with.
	module string
	cmd    *exec.Cmd
	done   chan error
}

// startChild runs path and feeds its output through logChild. Both output streams share one
// pipe: with the same *os.File for Stdout and Stderr, os/exec copies them from a single
// goroutine, so lines from the two never interleave.
func startChild(module, path string, args ...string) (*childProcess, error) {
	read, write, err := os.Pipe()
	if err != nil {
		return nil, err
	}

	cmd := exec.Command(path, args...)
	cmd.Stdout = write
	cmd.Stderr = write
	if err := cmd.Start(); err != nil {
		read.Close()
		write.Close()
		return nil, err
	}
	// The child holds the only remaining write end, so reading until EOF ends with it.
	write.Close()
	infof(moduleSupervisor, "started %s (pid %d)", path, cmd.Process.Pid)

	child := &childProcess{module: module, cmd: cmd, done: make(chan error, 1)}
	go logChild(module, read)
	go func() {
		child.done <- cmd.Wait()
		// Closing after the send lets a second reader see the exit without blocking, which
		// is what stop() relies on when the exit was already observed by run().
		close(child.done)
	}()
	return child, nil
}

// logChild relays one line at a time from a child. The child stamps its own lines and marks
// the non-info ones; both are replaced by this process's own columns, so all three
// processes end up in one format.
func logChild(module string, output io.ReadCloser) {
	defer output.Close()
	reader := bufio.NewReader(output)
	for {
		// ReadString rather than a Scanner: the protocol stack can emit long lines, and a
		// Scanner would silently give up on them at its buffer limit.
		line, err := reader.ReadString('\n')
		if line != "" {
			level, message := parseChildLine(line)
			if strings.TrimSpace(message) != "" {
				logLine(level, module, "%s", message)
			}
		}
		if err != nil {
			return
		}
	}
}

// exited returns the channel to watch, or nil when the child never started. A nil channel
// blocks forever in a select, which is exactly right: there is nothing to wait for.
func exited(c *childProcess) <-chan error {
	if c == nil {
		return nil
	}
	return c.done
}

// stop asks the child to exit and kills it if it does not. The signal matters for the
// bridge: it releases the USB claim on the way out.
func (c *childProcess) stop() {
	if c == nil || c.cmd.Process == nil {
		return
	}
	if err := c.cmd.Process.Signal(syscall.SIGTERM); err != nil {
		// Already gone. Waiting is still safe: done is closed once Wait returned.
		<-c.done
		return
	}
	select {
	case <-c.done:
	case <-time.After(stopTimeout):
		warnf(moduleSupervisor, "%s did not exit after SIGTERM, killing it", c.module)
		c.cmd.Process.Kill()
		<-c.done
	}
}

// exitReason renders what Wait returned, for a log line that reads well when there is no
// error, which is the normal case for a clean exit.
func exitReason(err error) string {
	if err == nil {
		return "no error"
	}
	return err.Error()
}

// waitOrStop sleeps for d and reports whether the run should continue.
func waitOrStop(ctx context.Context, d time.Duration) bool {
	select {
	case <-ctx.Done():
		return false
	case <-time.After(d):
		return true
	}
}
