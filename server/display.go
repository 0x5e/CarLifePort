package main

import "fmt"

// Display mode advertised to the phone and reported to the browser. The shape mirrors
// playport's DisplayState, which the unmodified web client reads from /api/display.
type displayState struct {
	Width    int
	Height   int
	FPS      int
	UIScale  int
	HEVC     bool
	MinEdge  int
	MaxEdge  int
	MinFPS   int
	MaxFPS   int
	Presets  []displayPreset
	OnChange func(width, height, fps int) error
}

type displayPreset struct {
	Label       string `json:"label"`
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	Orientation string `json:"orientation"`
}

// Presets offered by the web UI. Landscape only: CarLife head units are landscape and
// the protocol has no portrait negotiation.
var displayPresets = []displayPreset{
	{Label: "800 x 480", Width: 800, Height: 480, Orientation: "landscape"},
	{Label: "1024 x 600", Width: 1024, Height: 600, Orientation: "landscape"},
	{Label: "1280 x 720", Width: 1280, Height: 720, Orientation: "landscape"},
	{Label: "1920 x 720", Width: 1920, Height: 720, Orientation: "landscape"},
	{Label: "1920 x 1080", Width: 1920, Height: 1080, Orientation: "landscape"},
}

func newDisplayState(width, height, fps int, onChange func(int, int, int) error) *displayState {
	return &displayState{
		Width:    width,
		Height:   height,
		FPS:      fps,
		UIScale:  100,
		MinEdge:  480,
		MaxEdge:  3840,
		MinFPS:   24,
		MaxFPS:   60,
		Presets:  displayPresets,
		OnChange: onChange,
	}
}

func (d *displayState) orientation() string {
	if d.Height > d.Width {
		return "portrait"
	}
	return "landscape"
}

// validate returns an error message for the browser, or nil when the request is usable.
// CarLife only negotiates H.264, so a request for HEVC is rejected outright.
func (d *displayState) validate(width, height, fps int, hevc bool) error {
	if hevc {
		return fmt.Errorf("HEVC is not supported; CarLife only encodes H.264")
	}
	if width < d.MinEdge || width > d.MaxEdge || height < d.MinEdge || height > d.MaxEdge {
		return fmt.Errorf("width and height must be between %d and %d", d.MinEdge, d.MaxEdge)
	}
	if width%2 != 0 || height%2 != 0 {
		return fmt.Errorf("width and height must be even")
	}
	if fps < d.MinFPS || fps > d.MaxFPS {
		return fmt.Errorf("fps must be between %d and %d", d.MinFPS, d.MaxFPS)
	}
	return nil
}

func (d *displayState) apply(width, height, fps int) error {
	if err := d.OnChange(width, height, fps); err != nil {
		return err
	}
	d.Width, d.Height, d.FPS = width, height, fps
	return nil
}
