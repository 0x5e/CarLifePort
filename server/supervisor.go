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

// restartDelay is how long to wait before rebuilding the link after the link process exits.
// Reseating the cable needs a moment: the phone re-enumerates and only then does the system
// start the CarLife app again.
const restartDelay = 2 * time.Second

// stopTimeout bounds the wait for the child to exit after SIGTERM before it is killed.
const stopTimeout = 3 * time.Second

// checkSibling verifies that the process this binary starts is where it looks for it,
// before anything is sent to the phone.
func checkSibling(cfg config) error {
	info, err := os.Stat(cfg.linkPath)
	if err != nil {
		return fmt.Errorf("missing %s, run make first", cfg.linkPath)
	}
	if info.Mode().Perm()&0o111 == 0 {
		return fmt.Errorf("%s is not executable, run make first", cfg.linkPath)
	}
	return nil
}

// supervisor starts carlife-link next to this binary and restarts it whenever it exits, so
// that running a link is one process to start and one to stop.
//
// It is restarted rather than resumed in place because the link cannot be re-established
// inside a running process: CarLifeVehicleLib opens its six channels in its own setup, which
// also runs the adb forward commands this project does not use, and the phone side starts a
// fresh session on every accessory attach.
type supervisor struct {
	linkPath string
	linkArgs []string
}

func newSupervisor(cfg config) *supervisor {
	return &supervisor{
		linkPath: cfg.linkPath,
		// The link takes its flags from the command line; carlifed owns the values, so it
		// passes them explicitly instead of relying on the binary's own defaults.
		linkArgs: []string{
			"--socket", defaultSocketPath,
			"--channel", cfg.channel,
			"--cuid", cfg.cuid,
		},
	}
}

func (s *supervisor) run(ctx context.Context) {
	for {
		link, err := startChild(moduleLink, s.linkPath, s.linkArgs...)
		if err != nil {
			// Nothing is running, so there is nothing to wait for: back off and retry.
			errorf(moduleSupervisor, "cannot start %s: %v", s.linkPath, err)
			if !waitOrStop(ctx, restartDelay) {
				return
			}
			continue
		}

		select {
		case <-ctx.Done():
		case err := <-link.done:
			infof(moduleSupervisor, "carlife-link exited (%s), rebuilding the link", exitReason(err))
		}

		link.stop()
		if ctx.Err() != nil {
			return
		}
		infof(moduleSupervisor, "rebuilding the link in %s (Ctrl-C to stop)", restartDelay)
		if !waitOrStop(ctx, restartDelay) {
			return
		}
	}
}

// childProcess is a running child plus the channel that reports how it ended.
type childProcess struct {
	// module is the name the child's output is tagged with when the child does not name the
	// subsystem itself.
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

// logChild relays one line at a time from the child. The child stamps its own lines and
// names the subsystem each one came from; both are replaced by this process's own columns,
// so the link's output and this process's output end up in one format.
func logChild(module string, output io.ReadCloser) {
	defer output.Close()
	reader := bufio.NewReader(output)
	for {
		// ReadString rather than a Scanner: the protocol stack can emit long lines, and a
		// Scanner would silently give up on them at its buffer limit.
		line, err := reader.ReadString('\n')
		if line != "" {
			level, subsystem, message := parseChildLine(line)
			if subsystem == "" {
				subsystem = module
			}
			if strings.TrimSpace(message) != "" {
				logLine(level, subsystem, "%s", message)
			}
		}
		if err != nil {
			return
		}
	}
}

// stop asks the child to exit and kills it if it does not. The signal matters for the USB
// transport: it releases the USB claim on the way out.
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
