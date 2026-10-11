import { AudioEngine } from './audio';
import { InputController } from './input';
import type { HardKey, ServerMessage, WireMessage } from './protocol';
import { VideoPlayer } from './video';
import { Connection, type ConnectionState } from './ws';

function requiredElement<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) throw new Error(`missing #${id} element`);
  return element as T;
}

const canvas = requiredElement<HTMLCanvasElement>('screen');
const surface = requiredElement<HTMLDivElement>('surface');
const statusDot = requiredElement<HTMLSpanElement>('status-dot');
const statusText = requiredElement<HTMLSpanElement>('status-text');
const overlay = requiredElement<HTMLDivElement>('overlay');
const overlayTitle = requiredElement<HTMLHeadingElement>('overlay-title');
const overlayText = requiredElement<HTMLParagraphElement>('overlay-text');
const overlayEyebrow = requiredElement<HTMLParagraphElement>('overlay-eyebrow');
const errorBanner = requiredElement<HTMLDivElement>('error-banner');
const streamInfo = requiredElement<HTMLSpanElement>('stream-info');
const deviceNameLabel = requiredElement<HTMLSpanElement>('device-name');
const app = requiredElement<HTMLDivElement>('app');
const screenContainer = requiredElement<HTMLDivElement>('screen-container');
const viewerState = requiredElement<HTMLSpanElement>('viewer-state');
const audioMaster = requiredElement<HTMLInputElement>('audio-master');
const quickVolume = requiredElement<HTMLInputElement>('volume-quick');
const muteButton = requiredElement<HTMLButtonElement>('btn-mute');

let sessionActive = false;
let muted = false;
let audioUnlocked = false;
let framesPainted = 0;
let framesReceived = 0;
let bytesReceived = 0;
let lastFrameArrival = 0;
let latencySampleMs = 0;
let streamWidth = 0;
let streamHeight = 0;

function fitScreen(): void {
  const scale = Math.min(screenContainer.clientWidth / canvas.width, screenContainer.clientHeight / canvas.height);
  surface.style.width = `${canvas.width * scale}px`;
  surface.style.height = `${canvas.height * scale}px`;
}

new ResizeObserver(fitScreen).observe(screenContainer);

const video = new VideoPlayer(
  canvas,
  (message) => showError(message),
  () => {
    if (!video.isConfigured || !sessionActive) return;
    framesPainted += 1;
    const now = performance.now();
    if (lastFrameArrival > 0) latencySampleMs = Math.max(0, now - lastFrameArrival);
    overlay.classList.add('hidden');
    app.dataset.session = 'live';
    viewerState.textContent = 'LIVE';
  },
  // CarLife has no request-keyframe command, so a stalled decoder can only wait for the next frame.
  () => {},
  (width, height) => {
    streamWidth = width;
    streamHeight = height;
    fitScreen();
  },
);

const audio = new AudioEngine((message) => showError(message));

const connection = new Connection({
  onWire: (message: WireMessage) => {
    switch (message.kind) {
      case 'video-config':
        video.configure(message);
        break;
      case 'video-frame':
        framesReceived += 1;
        lastFrameArrival = performance.now();
        video.pushFrame(message);
        break;
      case 'audio-config':
        audio.handleConfig(message);
        break;
      case 'audio-packet':
        audio.handlePacket(message);
        break;
      case 'audio-stopped':
        audio.stopStream(message.streamType);
        break;
      case 'screen-active':
        break;
    }
  },
  onServer: (message: ServerMessage) => applyServerState(message),
  onState: (state: ConnectionState) => applyConnectionState(state),
  onBytes: (count) => {
    bytesReceived += count;
  },
});

const input = new InputController(canvas, (message) => connection.send(message));
input.attach();

function resetSession(): void {
  sessionActive = false;
  app.dataset.session = 'idle';
  canvas.inert = true;
  for (const button of document.querySelectorAll<HTMLButtonElement>('[data-remote]')) button.disabled = true;
  video.close();
  audio.stopAll();
  streamWidth = streamHeight = framesReceived = framesPainted = bytesReceived = lastFrameArrival = latencySampleMs = 0;
  streamInfo.textContent = '等待视频流';
  canvas.getContext('2d')?.clearRect(0, 0, canvas.width, canvas.height);
  overlay.classList.remove('hidden');
}

function applyServerState(message: ServerMessage): void {
  if (message.type !== 'hello' && message.type !== 'session') return;

  const active = message.sessionActive === true;
  const device = message.deviceName || '手机';
  deviceNameLabel.textContent = active ? device : '尚未连接';

  if (active) {
    setStatus('live', `${device} 已连接`);
    if (!sessionActive) {
      viewerState.textContent = 'STARTING';
      app.dataset.session = 'connected';
      setOverlay('已连接手机。', '正在取第一帧画面，稍等片刻。', 'ALMOST THERE');
    }
    sessionActive = true;
    canvas.inert = false;
    for (const button of document.querySelectorAll<HTMLButtonElement>('[data-remote]')) button.disabled = false;
    return;
  }

  resetSession();
  if (message.hostReady) {
    setStatus('idle', '车机已就绪');
    viewerState.textContent = 'STANDBY';
    setOverlay('车机已就绪。', '在手机上打开 CarLife 并确认连接，画面会自动出现。', 'STANDBY');
  } else {
    setStatus('idle', '等待车机');
    viewerState.textContent = 'WAITING';
    setOverlay('还没有连上手机。', '先用数据线连接手机并启动 usb-bridge 与 carlife-host。', 'WAITING FOR DEVICE');
  }
}

function applyConnectionState(state: ConnectionState): void {
  switch (state) {
    case 'connecting':
      setStatus('idle', '连接服务中…');
      break;
    case 'open':
      setStatus('idle', '服务已连接');
      break;
    case 'closed':
      resetSession();
      setStatus('idle', '重新连接中…');
      viewerState.textContent = 'RECONNECTING';
      setOverlay('正在恢复连接。', '与服务端的连接中断了，会自动重连。', 'RECONNECTING');
      break;
  }
}

function setStatus(kind: 'live' | 'idle', text: string): void {
  statusDot.dataset.state = kind;
  statusText.textContent = text;
}

function setOverlay(title: string, text: string, eyebrow = 'READY WHEN YOU ARE'): void {
  overlayTitle.textContent = title;
  overlayText.textContent = text;
  overlayEyebrow.textContent = eyebrow;
}

function showError(message: string): void {
  if (!message) {
    errorBanner.classList.add('hidden');
    requiredElement('error-text').textContent = '';
    return;
  }
  requiredElement('error-text').textContent = message;
  errorBanner.classList.remove('hidden');
}

function clickButton(id: string, handler: () => void): void {
  requiredElement<HTMLButtonElement>(id).addEventListener('click', (event) => {
    event.preventDefault();
    handler();
  });
}

/* ---------------------------------------------------------- remote control bar */

const remoteButtons: Array<[string, HardKey]> = [
  ['btn-up', 'up'],
  ['btn-down', 'down'],
  ['btn-left', 'left'],
  ['btn-right', 'right'],
  ['btn-ok', 'ok'],
  ['btn-back', 'back'],
  ['btn-home', 'home'],
  ['btn-prev', 'prev'],
  ['btn-media', 'media'],
  ['btn-next', 'next'],
  ['btn-navi', 'navi'],
  ['btn-tel', 'tel'],
];

for (const [id, key] of remoteButtons) {
  clickButton(id, () => connection.send({ type: 'key', key }));
}

clickButton('btn-foreground', () => {
  connection.send({ type: 'foreground' });
  toast('已请求手机回到前台');
});
clickButton('btn-fullscreen', () => void toggleFullscreen());

async function toggleFullscreen(): Promise<void> {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else if (document.documentElement.requestFullscreen) await document.documentElement.requestFullscreen();
    else toast('当前浏览器不支持全屏，可以改用专注模式。');
  } catch {
    toast('无法进入全屏，可以改用专注模式。');
  }
}

document.addEventListener('fullscreenchange', () => {
  requiredElement('btn-fullscreen').setAttribute('aria-label', document.fullscreenElement ? '退出全屏' : '进入全屏');
});

let toastTimer = 0;
function toast(message: string): void {
  const element = requiredElement('toast');
  element.textContent = message;
  element.hidden = false;
  window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    element.hidden = true;
  }, 2800);
}

function toggleFocus(): void {
  const focused = app.classList.toggle('focus-mode');
  requiredElement('btn-focus').setAttribute('aria-pressed', String(focused));
  requiredElement('btn-exit-focus').hidden = !focused;
  requiredElement<HTMLButtonElement>(focused ? 'btn-exit-focus' : 'btn-focus').focus();
}
clickButton('btn-focus', toggleFocus);
clickButton('btn-exit-focus', toggleFocus);
clickButton('btn-error-close', () => showError(''));
clickButton('btn-viewer', () => canvas.focus());

/* ------------------------------------------------------------------- volume */

muteButton.addEventListener('click', () => {
  muted = !muted;
  updateMasterVolume();
});

async function unlockAudio(): Promise<void> {
  if (audioUnlocked) return;
  try {
    await audio.unlock();
    audioUnlocked = true;
    requiredElement('audio-unlock-hint').hidden = true;
  } catch {
    // Retry on the next user gesture.
  }
}

window.addEventListener('pointerdown', () => void unlockAudio(), { once: false });
window.addEventListener('keydown', () => void unlockAudio(), { once: false });

function updateMasterVolume(): void {
  audio.setVolume(muted ? 0 : Number(audioMaster.value) / 100);
  quickVolume.value = audioMaster.value;
  requiredElement('volume-value').textContent = muted ? '已静音' : `${audioMaster.value}%`;
  requiredElement('audio-master-value').textContent = muted ? `${audioMaster.value}% · 静音` : `${audioMaster.value}%`;
  muteButton.setAttribute('aria-pressed', String(muted));
  muteButton.setAttribute('aria-label', muted ? '取消静音' : '静音');
  muteButton.title = muted ? '取消静音' : '静音';
  document.getElementById('mute-icon')?.setAttribute('href', muted ? '#i-mute' : '#i-audio');
}

const savedMaster = Number(localStorage.getItem('carlife-master') ?? '100');
audioMaster.value = String(Number.isFinite(savedMaster) ? savedMaster : 100);
updateMasterVolume();

audioMaster.addEventListener('input', () => {
  muted = false;
  updateMasterVolume();
  localStorage.setItem('carlife-master', audioMaster.value);
});
quickVolume.addEventListener('input', () => {
  audioMaster.value = quickVolume.value;
  audioMaster.dispatchEvent(new Event('input'));
});

/* --------------------------------------------------------- display settings */

interface DisplayPreset {
  width: number;
  height: number;
}

interface DisplayInfo {
  width: number;
  height: number;
  fps: number;
  presets: DisplayPreset[];
}

const displayPanel = requiredElement<HTMLDialogElement>('display-panel');
const displayPresets = requiredElement<HTMLDivElement>('display-presets');
const displayWidthInput = requiredElement<HTMLInputElement>('display-width');
const displayHeightInput = requiredElement<HTMLInputElement>('display-height');
const displayFps = requiredElement<HTMLSelectElement>('display-fps');
const displayStatus = requiredElement<HTMLParagraphElement>('display-status');
const displayApply = requiredElement<HTMLButtonElement>('btn-display-apply');
const displayForm = requiredElement<HTMLFormElement>('display-form');
const displayFields = requiredElement<HTMLFieldSetElement>('display-fields');

function renderPresets(presets: DisplayPreset[]): void {
  displayPresets.replaceChildren();
  for (const preset of presets) {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.width = String(preset.width);
    button.dataset.height = String(preset.height);
    button.setAttribute('aria-pressed', 'false');
    const shape = document.createElement('span');
    shape.className = `preset-screen ${preset.height > preset.width ? 'portrait' : preset.width / preset.height > 2 ? 'ultrawide' : ''}`;
    shape.setAttribute('aria-hidden', 'true');
    const copy = document.createElement('span');
    copy.className = 'preset-copy';
    copy.textContent = `${preset.width} × ${preset.height}`;
    const shape2 = document.createElement('small');
    shape2.textContent = preset.height > preset.width ? '竖屏' : preset.width / preset.height > 2 ? '带鱼屏' : '横屏';
    copy.append(shape2);
    button.append(shape, copy);
    button.addEventListener('click', () => {
      displayWidthInput.value = String(preset.width);
      displayHeightInput.value = String(preset.height);
      selectMatchingPreset();
    });
    displayPresets.append(button);
  }
}

function selectMatchingPreset(): void {
  for (const button of displayPresets.querySelectorAll('button')) {
    const selected = button.dataset.width === displayWidthInput.value && button.dataset.height === displayHeightInput.value;
    button.classList.toggle('selected', selected);
    button.setAttribute('aria-pressed', String(selected));
  }
}
displayWidthInput.addEventListener('input', selectMatchingPreset);
displayHeightInput.addEventListener('input', selectMatchingPreset);

async function refreshDisplayInfo(): Promise<void> {
  displayFields.disabled = true;
  displayApply.disabled = true;
  displayStatus.dataset.error = 'false';
  displayStatus.textContent = '正在读取当前画面设置…';
  try {
    const response = await fetch('/api/display');
    if (!response.ok) throw new Error(`request failed (${response.status})`);
    const info = (await response.json()) as DisplayInfo;
    renderPresets(info.presets);
    displayWidthInput.value = String(info.width);
    displayHeightInput.value = String(info.height);
    displayFps.value = String(info.fps);
    selectMatchingPreset();
    displayFields.disabled = false;
    displayApply.disabled = false;
    displayStatus.textContent = `当前 ${info.width}×${info.height} @ ${info.fps} fps，H.264`;
  } catch {
    displayStatus.dataset.error = 'true';
    displayStatus.textContent = '读取设置失败，请确认服务端仍在运行。';
  }
}

async function applyDisplay(): Promise<void> {
  const width = Number(displayWidthInput.value);
  const height = Number(displayHeightInput.value);
  const fps = Number(displayFps.value);
  if (displayApply.disabled || !displayForm.reportValidity()) return;
  displayApply.disabled = true;
  displayFields.disabled = true;
  displayStatus.dataset.error = 'false';
  displayStatus.textContent = '正在应用…';
  try {
    const response = await fetch('/api/display', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ width, height, fps }),
    });
    const body = (await response.json().catch(() => ({}))) as { ok?: boolean };
    const ok = response.ok && body.ok === true;
    displayStatus.dataset.error = String(!ok);
    displayStatus.textContent = ok ? '已应用，画面会在几秒内恢复。' : '应用失败，请检查分辨率是否在范围内。';
  } catch (error) {
    displayStatus.dataset.error = 'true';
    displayStatus.textContent = `应用失败：${String(error)}`;
  } finally {
    displayApply.disabled = false;
    displayFields.disabled = false;
  }
}

clickButton('btn-display', () => {
  displayPanel.showModal();
  void refreshDisplayInfo();
});
clickButton('btn-display-close', () => displayPanel.close());
displayForm.addEventListener('submit', (event) => {
  event.preventDefault();
  void applyDisplay();
});

/* ------------------------------------------------------------- audio panel */

const audioPanel = requiredElement<HTMLDialogElement>('audio-panel');
const audioStreams = requiredElement<HTMLDivElement>('audio-streams');
const audioStatus = requiredElement<HTMLParagraphElement>('audio-status');

function streamGainKey(name: string): string {
  return `carlife-gain-${name}`;
}

function ensureStreamSliders(): void {
  const names = audio.activeStreamNames();
  const existing = new Set(
    Array.from(audioStreams.querySelectorAll<HTMLElement>('[data-stream]'), (element) => element.dataset.stream ?? ''),
  );
  audioStatus.textContent =
    names.length === 0 ? '还没有声音。手机上开始播放音乐或导航播报后，这里会出现对应的音量调节。' : '';
  if (names.length === existing.size && names.every((name) => existing.has(name))) return;
  audioStreams.replaceChildren();
  for (const name of names) {
    const row = document.createElement('div');
    row.className = 'audio-stream-row';
    row.dataset.stream = name;
    const label = document.createElement('label');
    label.textContent = name;
    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = '0';
    slider.max = '100';
    slider.id = `audio-stream-${audioStreams.childElementCount}`;
    label.htmlFor = slider.id;
    const output = document.createElement('output');
    output.htmlFor = slider.id;
    const stored = Number(localStorage.getItem(streamGainKey(name)) ?? '100');
    slider.value = String(Number.isFinite(stored) ? stored : 100);
    output.textContent = `${slider.value}%`;
    audio.setStreamGain(name, Number(slider.value) / 100);
    slider.addEventListener('input', () => {
      audio.setStreamGain(name, Number(slider.value) / 100);
      localStorage.setItem(streamGainKey(name), slider.value);
      output.textContent = `${slider.value}%`;
    });
    row.append(label, output, slider);
    audioStreams.append(row);
  }
}

window.setInterval(ensureStreamSliders, 1000);

clickButton('btn-audio', () => {
  audioPanel.showModal();
  ensureStreamSliders();
});
clickButton('btn-audio-close', () => audioPanel.close());

const helpPanel = requiredElement<HTMLDialogElement>('help-panel');
clickButton('btn-help', () => helpPanel.showModal());
clickButton('btn-setup', () => helpPanel.showModal());
clickButton('btn-help-close', () => helpPanel.close());

for (const [panel, trigger] of [
  [displayPanel, 'btn-display'],
  [audioPanel, 'btn-audio'],
  [helpPanel, 'btn-help'],
] as const) {
  panel.addEventListener('click', (event) => {
    const rect = panel.getBoundingClientRect();
    if (
      event.target === panel &&
      (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom)
    ) {
      panel.close();
    }
  });
  panel.addEventListener('close', () => requiredElement(trigger).classList.remove('active'));
  requiredElement(trigger).addEventListener('click', () => requiredElement(trigger).classList.add('active'));
}

/* -------------------------------------------------------------- status bar */

let statsTick = performance.now();

function updateStats(): void {
  const now = performance.now();
  const elapsedSeconds = Math.max(0.001, (now - statsTick) / 1000);
  statsTick = now;
  const fps = framesPainted / elapsedSeconds;
  const mbps = (bytesReceived * 8) / 1_000_000 / elapsedSeconds;
  const dropped = Math.max(0, framesReceived - framesPainted);
  const queue = video.decodeQueueSize;
  const latency = latencySampleMs;
  framesPainted = 0;
  framesReceived = 0;
  bytesReceived = 0;
  if (streamWidth === 0) {
    streamInfo.textContent = '等待视频流';
    return;
  }
  const parts = [`${streamWidth} × ${streamHeight}`];
  parts.push(`${fps.toFixed(0)} fps`);
  parts.push(`${mbps.toFixed(1)} Mbps`);
  if (dropped > 0) parts.push(`丢弃 ${dropped}`);
  if (queue > 2) parts.push(`队列 ${queue}`);
  if (latency > 0 && latency < 1000) parts.push(`解码 ~${latency.toFixed(0)} ms`);
  streamInfo.textContent = parts.join(' · ');
}

window.setInterval(updateStats, 500);

if (typeof VideoDecoder === 'undefined') {
  showError('视频解码需要 WebCodecs。请用 Chrome 或 Edge 打开本页面。');
}

connection.connect();
