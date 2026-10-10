package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8080", "HTTP 监听地址")
	hostSocket := flag.String("host-socket", "/tmp/carlife-host.sock", "carlife-host 的 Unix socket 路径")
	webDir := flag.String("web", "web/dist", "前端静态文件目录")
	width := flag.Int("width", 1280, "请求手机编码的视频宽度")
	height := flag.Int("height", 720, "请求手机编码的视频高度")
	fps := flag.Int("fps", 30, "请求手机编码的视频帧率")
	flag.Parse()

	log.SetFlags(log.Ltime)

	if _, err := os.Stat(*webDir); err != nil {
		log.Printf("提示：%s 不存在，先在 web/ 目录执行 npm install && npm run build", *webDir)
	}

	hub := newHub()
	host := newHostClient(*hostSocket)
	sess := newSession(host, hub, *width, *height, *fps)
	hub.session = sess
	host.onEvent = sess.onHostEvent
	host.onState = func(connected bool) {
		sess.mu.Lock()
		sess.hostReady = connected
		sess.mu.Unlock()
		sess.broadcastState()
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go host.run(ctx)

	server := &http.Server{Addr: *addr, Handler: hub.Routes(*webDir)}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	log.Printf("CarLife 车机端服务已启动")
	log.Printf("  浏览器打开   http://%s", *addr)
	log.Printf("  前端目录     %s", *webDir)
	log.Printf("  等待宿主进程 %s", *hostSocket)

	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("HTTP 服务退出：%v", err)
	}
	log.Printf("已停止")
}
