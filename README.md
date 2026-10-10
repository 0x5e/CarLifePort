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

### 1. 原生依赖与车机协议栈

```sh
./scripts/build-native.sh          # protobuf 2.5.0 + libcarlifevehicle.dylib → .build/
make -C host                       # → host/carlife-host
make -C tools/usb-bridge           # → tools/usb-bridge/carlife-usb-bridge
```

`build-native.sh` 会对 protobuf 2.5.0 打一个 3 行的补丁（补上 `__aarch64__` 分支，
原版不认识 Apple Silicon），其余源文件原样编译。产物全部落在 `.build/`，不进入版本库。

### 2. 后端与前端

```sh
cd server && go build -o carlifed .    # → server/carlifed
cd web && npm install && npm run build # → web/dist
```

## 运行

按顺序启动四个进程：

```sh
# 1. 手机用数据线接上 Mac，先停掉 adb server（否则 AOA 握手时设备被 adb 占用）
adb kill-server
adb shell svc usb setFunctions          # 清掉可能残留的 accessory 模式

# 2. USB 桥接
./tools/usb-bridge/carlife-usb-bridge

# 3. 车机协议栈宿主
./host/carlife-host

# 4. 后端（在仓库根目录运行，默认读 web/dist）
./server/carlifed

# 5. 浏览器打开 http://127.0.0.1:8080
```

手机端打开 CarLife App 并确认连接，画面会自动出现。

### 常用参数

```sh
carlife-usb-bridge --list               # 枚举当前 USB 设备
carlife-host --socket /tmp/host.sock    # 换 socket 路径
carlifed --addr 127.0.0.1:8080 \
         --host-socket /tmp/host.sock \
         --web web/dist \
         --width 1280 --height 720 --fps 30
```

## 已知限制

- 只实现有线（AOA）通路，无线 CarLife 未实现。
- 触摸是单点，CarLife 协议本身不支持多指。
- 视频只有 H.264；音频按 16 位 PCM 处理。
- `carlife-host` 在 `connectionSetup()` 失败后不会自动重试，需重启进程。
