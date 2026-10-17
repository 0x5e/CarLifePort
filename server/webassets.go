package main

import (
	"embed"
	"io/fs"
)

// The browser client is compiled into this binary. The Makefile stages the built frontend
// (vendor/playport/web, unmodified) onto server/web before the Go build; nothing is read
// from disk at run time, so carlifed carries the UI itself. server/web is generated and not
// tracked, which keeps the vendored copy the only one in the repository.
//
//go:embed all:web
var embeddedWeb embed.FS

// webRoot returns the embedded assets rooted at the asset directory, so the HTTP handler
// serves /index.html rather than /web/index.html.
func webRoot() fs.FS {
	root, err := fs.Sub(embeddedWeb, "web")
	if err != nil {
		// fs.Sub fails only for a path that cannot be a root, and this one is a constant
		// that the embed directive already resolved at build time.
		panic(err)
	}
	return root
}
