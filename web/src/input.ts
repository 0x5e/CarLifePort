import type { ClientMessage, HardKey } from './protocol';

/** Keyboard key to CarLife head-unit hard key mapping. */
const KEY_MAP: Record<string, HardKey> = {
  ArrowUp: 'up',
  ArrowDown: 'down',
  ArrowLeft: 'left',
  ArrowRight: 'right',
  Enter: 'ok',
  ' ': 'ok',
  Escape: 'back',
  Backspace: 'back',
  Home: 'home',
  MediaPlayPause: 'ok',
  MediaTrackNext: 'next',
  MediaTrackPrevious: 'prev',
};

/** Map browser pointer and keyboard input to CarLife single-touch and hard keys. */
export class InputController {
  private down = false;
  private pendingFlush = 0;
  private x = 0;
  private y = 0;

  constructor(
    private readonly surface: HTMLElement,
    private readonly send: (message: ClientMessage) => void,
  ) {}

  attach(): void {
    const surface = this.surface;
    surface.style.touchAction = 'none';
    surface.addEventListener('pointerdown', this.onPointerDown, { passive: false });
    surface.addEventListener('pointermove', this.onPointerMove, { passive: false });
    surface.addEventListener('pointerup', this.onPointerUp, { passive: false });
    surface.addEventListener('pointercancel', this.onPointerUp, { passive: false });
    surface.addEventListener('pointerleave', this.onPointerUp, { passive: false });
    window.addEventListener('keydown', this.onKeyDown);
  }

  detach(): void {
    const surface = this.surface;
    surface.removeEventListener('pointerdown', this.onPointerDown);
    surface.removeEventListener('pointermove', this.onPointerMove);
    surface.removeEventListener('pointerup', this.onPointerUp);
    surface.removeEventListener('pointercancel', this.onPointerUp);
    surface.removeEventListener('pointerleave', this.onPointerUp);
    window.removeEventListener('keydown', this.onKeyDown);
  }

  private readonly onPointerDown = (event: PointerEvent): void => {
    event.preventDefault();
    this.surface.focus({ preventScroll: true });
    this.surface.setPointerCapture(event.pointerId);
    this.down = true;
    this.x = this.normalizeX(event.clientX);
    this.y = this.normalizeY(event.clientY);
    this.scheduleFlush();
  };

  private readonly onPointerMove = (event: PointerEvent): void => {
    if (!this.down) return;
    event.preventDefault();
    this.x = this.normalizeX(event.clientX);
    this.y = this.normalizeY(event.clientY);
    this.scheduleFlush();
  };

  private readonly onPointerUp = (event: PointerEvent): void => {
    if (!this.down) return;
    event.preventDefault();
    this.down = false;
    this.scheduleFlush();
  };

  private readonly onKeyDown = (event: KeyboardEvent): void => {
    if (this.surface.inert || event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey) return;
    if (document.querySelector('dialog[open]')) return;
    if (
      event.target instanceof Element &&
      (event.target.closest('input, select, textarea, [contenteditable="true"]') ||
        ((event.key === 'Enter' || event.key === ' ') && event.target.closest('button, a')))
    ) {
      return;
    }
    const key = KEY_MAP[event.key];
    if (!key) return;
    event.preventDefault();
    this.send({ type: 'key', key });
  };

  private scheduleFlush(): void {
    if (this.pendingFlush !== 0) return;
    this.pendingFlush = window.requestAnimationFrame(() => {
      this.pendingFlush = 0;
      this.send({ type: 'touch', x: this.x, y: this.y, down: this.down });
    });
  }

  private normalizeX(clientX: number): number {
    const rect = this.surface.getBoundingClientRect();
    if (rect.width <= 0) return 0;
    return clamp01((clientX - rect.left) / rect.width);
  }

  private normalizeY(clientY: number): number {
    const rect = this.surface.getBoundingClientRect();
    if (rect.height <= 0) return 0;
    return clamp01((clientY - rect.top) / rect.height);
  }
}

function clamp01(value: number): number {
  return Math.min(1, Math.max(0, value));
}
