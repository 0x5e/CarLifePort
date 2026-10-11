# CarLifePort

CarLife 车机端（web 版）。服务端跑车机侧协议栈，浏览器用 WebCodecs 当屏幕。

参考 [playport](https://github.com/youcci/playport) 的分层方式：车机协议在服务端，浏览器只负责解码显示。
CarLife 协议栈用的是百度的开源车机库 [CarLifeVehicleLib](./vendor/carlife)（Apache-2.0）。

## 架构

```
手机 ──USB(AOA)──► carlife-usb-bridge ──TCP(127.0.0.1)──► carlife-host ──Unix socket──► carlifed ──WebSocket──► 浏览器
      CarLife 协议      tools/usb-bridge      7200/8200/9200       host/            server/          web/
                                            9201/9202/9300
```

四段职责：

| 组件 | 语言 | 职责 |
|---|---|---|
| `tools/usb-bridge` | C | AOA 握手、USB 帧与六条本地 TCP 通道互转 |
| `host` | C++ | 链接 `libcarlifevehicle`，跑协议栈，把回调数据转成帧 |
| `server` | Go | 会话状态机、H.264 拆包、浏览器 WebSocket 服务 |
| `web` | TypeScript | WebCodecs 解码、Web Audio 播放、触摸与按键上行 |

车辆侧各通道固定端口（与库的 `SOCKET_*_LOCALHOST_PORT` 对齐）：

| 通道 | 车机侧 | 手机侧 |
|---|---|---|
| cmd | 7200 | 7240 |
| video | 8200 | 8240 |
| audio | 9200 | 9240 |
| tts | 9201 | 9241 |
| vr | 9202 | 9242 |
| touch | 9300 | 9340 |

## 构建

```sh
./scripts/build.sh          # 全部构建；已存在的原生库自动跳过
./scripts/build.sh --force  # 连原生库一起重建
```

按顺序构建五段，产物均不进入版本库：

| 步骤 | 产物 |
|---|---|
| 原生库（protobuf 2.5.0 + CarLife 协议栈） | `.build/lib/libcarlifevehicle.dylib` |
| USB 桥接层 | `tools/usb-bridge/carlife-usb-bridge` |
| 车机协议栈宿主 | `host/carlife-host` |
| Go 后端 | `server/carlifed` |
| 前端 | `web/dist/` |

原生库要编译 protobuf 与 69 个 C++ 源文件，首次构建需数分钟；产物已存在时跳过。
`build-native.sh` 会给 protobuf 2.5.0 打一个 3 行补丁（补上 `__aarch64__` 分支，
原版不认识 Apple Silicon），其余源文件原样编译。

## 运行

```sh
./scripts/run.sh
```

脚本依次做三件事：检查桌面版微信是否在运行（它独占 USB 设备，检测到会直接中止并
提示）→ 复位手机 USB 模式（清 accessory 残留、停掉 adb server）→ 启动
`carlife-usb-bridge`、`carlife-host`、`carlifed`。

三个进程的日志带 `[bridge]` / `[host]` / `[server]` 前缀输出到同一终端；任一进程
退出时其余进程一并停止，Ctrl-C 同样停止全部。

然后浏览器打开 <http://127.0.0.1:8080>，手机端打开 CarLife App 并确认连接，画面
自动出现。

进程之间有启动顺序依赖，`run.sh` 已处理：`carlife-host` 会等 `127.0.0.1:7200`
可连接后才调用 `connectionSetup()`，在此之前它不监听自己的 socket、也不发状态帧。
手工单独启动时必须遵守这个顺序。

### 单独运行某个组件

```sh
tools/usb-bridge/carlife-usb-bridge --list   # 枚举 USB 设备，并显示挑中的是哪个
tools/usb-bridge/carlife-usb-bridge          # 只跑桥接层
host/carlife-host -v                         # 只跑宿主；默认静音，-v 保留库日志
server/carlifed --addr 127.0.0.1:8080 \
                --host-socket /tmp/carlife-host.sock \
                --web web/dist \
                --width 1280 --height 720 --fps 30
cd web && npm run dev                        # 前端热更新，代理 /ws 与 /api 到 8080
```

改前端代码时用 `npm run dev`（vite 开发服务器）。它把 `/ws` 与 `/api` 代理到
8080，因此仍需另起 `carlifed`，不要用 `web/dist` 调试。

## 已知限制

- 只实现有线（AOA）通路，无线 CarLife 未实现。
- 触摸是单点，CarLife 协议本身不支持多指。
- 视频只有 H.264；音频按 16 位 PCM 处理。
- `carlife-host` 对 `connectionSetup()` 最多重试 3 次；仍失败需重启进程。
