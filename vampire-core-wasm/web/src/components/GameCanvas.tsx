import { useRef, useEffect, MutableRefObject } from 'react';
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

const W = 800, H = 600;
const WORLD_W = 2000, WORLD_H = 2000;
const SX = W / WORLD_W, SY = H / WORLD_H;

const TAU = Math.PI * 2;

// Mirrors Config::AURA_FLASH_TIME. Only used to normalise the flash timer into
// a 1 -> 0 ramp for the fade; the cadence itself is entirely the engine's.
const AURA_FLASH_TIME = 0.30;

// Enemy colour, premultiplied into a 32-bit little-endian ABGR word once.
const ENEMY_RGBA = (255 << 24) | (0x50 << 16) | (0x50 << 8) | 0xe0;  // #e05050
const BG_RGBA    = (255 << 24) | (0x0f << 16) | (0x0a << 8) | 0x0a;  // #0a0a0f

export default function GameCanvas({ engine, module, wasmReady, apiRef }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current!;
    const ctx    = canvas.getContext('2d')!;

    if (!wasmReady) {
      ctx.fillStyle = '#0a0a0f';
      ctx.fillRect(0, 0, W, H);
      ctx.fillStyle = '#444';
      ctx.font = '16px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Waiting for Wasm build...', W / 2, H / 2);
      apiRef.current = null;
      return;
    }

    // One reusable pixel buffer. Allocating this per frame would hand the GC
    // 1.9 MB every 16 ms and produce collection pauses that look exactly like
    // the simulation stutter this demo exists to measure.
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
        const x = (heap[iX + i] * SX) | 0;
        const y = (heap[iY + i] * SY) | 0;
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
      // things here, so paths are free and buy proper anti-aliased circles.
      // putImageData ignores the existing canvas contents, so this must come
      // after it or it would be erased.
      const px = eng.getPlayerX() * SX;
      const py = eng.getPlayerY() * SY;
      const ar = eng.getAuraRadius() * SX;

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
  }, [wasmReady, engine, module, apiRef]);

  return <canvas ref={canvasRef} width={W} height={H} className="game-canvas" />;
}
