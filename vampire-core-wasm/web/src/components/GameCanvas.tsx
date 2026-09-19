import { useRef, useEffect, useState, MutableRefObject } from 'react';
import type { EngineHandle, WasmModule } from '../App';

interface Props {
  engine:    MutableRefObject<EngineHandle | null>;
  module:    MutableRefObject<WasmModule | null>;
  wasmReady: boolean;
  // App owns the single rAF loop and calls draw() once per tick, so the canvas
  // never renders a half-updated world and there is only one callback
  // competing for the frame budget.
  apiRef:    MutableRefObject<{ draw: () => void } | null>;
}

const WORLD_W = 2000, WORLD_H = 2000;

// Fallback until the ResizeObserver reports the real box. Also the floor the
// backing store is clamped to, so a collapsed column can never produce a
// zero-sized ImageData (which throws).
const MIN_W = 320, MIN_H = 180;

const TAU = Math.PI * 2;

const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);

// Mirrors Config::AURA_FLASH_TIME. Only used to normalise the flash timer into
// a 1 -> 0 ramp for the fade; the cadence itself is entirely the engine's.
const AURA_FLASH_TIME = 0.30;

// Enemy colour, premultiplied into a 32-bit little-endian ABGR word once.
const ENEMY_RGBA = (255 << 24) | (0x50 << 16) | (0x50 << 8) | 0xe0;  // #e05050
const BG_RGBA    = (255 << 24) | (0x0c << 16) | (0x08 << 8) | 0x06;  // #06080c

export default function GameCanvas({ engine, module, wasmReady, apiRef }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const boxRef    = useRef<HTMLDivElement>(null);

  // The canvas fills the stage column rather than sitting at a fixed 800x600.
  // The backing store is resized to match so the raw pixel writes below stay
  // 1:1 with device pixels -- scaling an 800x600 buffer up would blur a field
  // whose entire point is that each enemy is a crisp 2x2 block.
  const [size, setSize] = useState({ w: MIN_W, h: MIN_H });

  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const observer = new ResizeObserver(entries => {
      const rect = entries[0].contentRect;
      // Rounded to whole pixels: a fractional box would otherwise re-fire the
      // effect on every sub-pixel layout wobble and rebuild the ImageData.
      const w = Math.max(MIN_W, Math.round(rect.width));
      const h = Math.max(MIN_H, Math.round(rect.height));
      setSize(prev => (prev.w === w && prev.h === h ? prev : { w, h }));
    });
    observer.observe(box);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current!;
    const ctx    = canvas.getContext('2d')!;
    const W = size.w, H = size.h;

    // ONE scale for both axes. The world is square and the stage is not, so a
    // per-axis fit would squash it -- and the aura, a circle in world space,
    // would have to be drawn as an ellipse to keep its kill radius honest.
    // Scaling to cover instead keeps circles circular and leaves no dead
    // space; what it costs is that the stage shows a window onto the world
    // rather than all of it, which is what the camera below is for.
    const S = Math.max(W / WORLD_W, H / WORLD_H);
    const VIEW_W = W / S, VIEW_H = H / S;   // visible world extent

    if (!wasmReady) {
      ctx.fillStyle = '#06080c';
      ctx.fillRect(0, 0, W, H);
      ctx.fillStyle = '#444';
      ctx.font = '16px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Waiting for Wasm build...', W / 2, H / 2);
      apiRef.current = null;
      return;
    }

    // One reusable pixel buffer. Allocating this per frame would hand the GC
    // a couple of MB every 16 ms and produce collection pauses that look
    // exactly like the simulation stutter this demo exists to measure. It is
    // rebuilt only when the stage is actually resized.
    const image = ctx.createImageData(W, H);
    const pixels = new Uint32Array(image.data.buffer);

    const draw = () => {
      const eng = engine.current;
      const mod = module.current;
      if (!eng || !mod) return;

      const count = eng.getEntityCount();
      // Re-read every frame: setEnemyCount() reallocates, and any Wasm heap
      // growth invalidates both these addresses and the HEAPF32 view.
      const ptrX  = eng.getPosXPtr();
      const ptrY  = eng.getPosYPtr();
      if (!ptrX || !ptrY) return;

      const heap = mod.HEAPF32;
      const iX   = ptrX >>> 2;
      const iY   = ptrY >>> 2;

      pixels.fill(BG_RGBA);

      // Camera: centred on the player, clamped so it never scrolls past the
      // world edge. Without the clamp the field would appear to drift off into
      // empty space whenever the player walked into a corner; with it, the
      // edge of the enemy field IS the edge of the world.
      const camX = clamp(eng.getPlayerX() - VIEW_W / 2, 0, Math.max(0, WORLD_W - VIEW_W));
      const camY = clamp(eng.getPlayerY() - VIEW_H / 2, 0, Math.max(0, WORLD_H - VIEW_H));

      // Direct pixel writes instead of beginPath/arc/fill per entity.
      //
      // The previous renderer issued 10,000 separate path operations per frame.
      // Canvas2D path setup and anti-aliased circle rasterisation are not free,
      // and at that count the draw cost exceeded the C++ tick by a wide margin
      // -- so the on-screen frame time was dominated by the renderer, and
      // toggling BruteForce/QuadTree barely moved the displayed number even
      // though the simulation cost changed by 20x.
      //
      // A 2x2 block per enemy keeps them visible while making the whole pass a
      // linear write over one typed array.
      for (let i = 0; i < count; ++i) {
        const x = ((heap[iX + i] - camX) * S) | 0;
        const y = ((heap[iY + i] - camY) * S) | 0;
        if (x < 0 || x >= W - 1 || y < 0 || y >= H - 1) continue;
        const p = y * W + x;
        pixels[p]         = ENEMY_RGBA;
        pixels[p + 1]     = ENEMY_RGBA;
        pixels[p + W]     = ENEMY_RGBA;
        pixels[p + W + 1] = ENEMY_RGBA;
      }

      ctx.putImageData(image, 0, 0);

      // --- player + aura, drawn with Canvas2D paths on top of the blit -------
      //
      // The entity field is written as raw pixels because there are 10,000 of
      // them and path ops would dominate the frame. There are exactly two
      // things here, so paths are free and buy proper anti-aliased curves.
      // putImageData ignores the existing canvas contents, so this must come
      // after it or it would be erased.
      //
      const px = (eng.getPlayerX() - camX) * S;
      const py = (eng.getPlayerY() - camY) * S;
      const ar = eng.getAuraRadius() * S;

      // World bounds, so the player can tell an edge from an empty patch. Only
      // the visible part of the rect is stroked; the rest is clipped for free.
      ctx.strokeStyle = 'rgba(120,140,180,0.18)';
      ctx.lineWidth = 1;
      ctx.strokeRect(-camX * S, -camY * S, WORLD_W * S, WORLD_H * S);

      // Charge ring: opacity ramps from nothing to full over the 2 s cadence,
      // so the pulse is readable as an approaching event rather than an
      // unexplained flash. Phase comes from the engine, not a JS timer -- a
      // separate timer would drift out of step the moment a frame is dropped,
      // which is precisely when the demo is under the load worth watching.
      const phase = eng.getAuraPhase();
      ctx.beginPath();
      ctx.arc(px, py, ar, 0, TAU);
      ctx.strokeStyle = `rgba(150,190,255,${0.08 + 0.30 * phase})`;
      ctx.lineWidth = 1 + 1.5 * phase;
      ctx.stroke();

      // Pulse flash: a bright ring that expands and fades over AURA_FLASH_TIME.
      const flash = eng.getAuraFlash();
      if (flash > 0) {
        const k = flash / AURA_FLASH_TIME;          // 1 -> 0
        ctx.beginPath();
        ctx.arc(px, py, ar * (1 + 0.12 * (1 - k)), 0, TAU);
        ctx.strokeStyle = `rgba(210,235,255,${0.85 * k})`;
        ctx.lineWidth = 3 * k + 1;
        ctx.stroke();

        ctx.beginPath();
        ctx.arc(px, py, ar, 0, TAU);
        ctx.fillStyle = `rgba(120,170,255,${0.12 * k})`;
        ctx.fill();
      }

      // Player: a white dot. Given a dark red enemy field, white is the only
      // fill that stays findable when a few thousand enemies pile onto it.
      ctx.beginPath();
      ctx.arc(px, py, 5, 0, TAU);
      ctx.fillStyle = '#ffffff';
      ctx.fill();
      ctx.strokeStyle = 'rgba(0,0,0,0.8)';
      ctx.lineWidth = 1.5;
      ctx.stroke();
    };

    apiRef.current = { draw };
    draw();
    return () => { apiRef.current = null; };
  }, [wasmReady, engine, module, apiRef, size.w, size.h]);

  return (
    <div className="game-canvas-box" ref={boxRef}>
      <canvas ref={canvasRef} width={size.w} height={size.h} className="game-canvas" />
    </div>
  );
}
