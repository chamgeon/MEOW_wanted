# Moved — Wasm output now lives in `web/public/`

The Emscripten output used to be copied here. It now goes to `web/public/`
instead, because Vite serves `public/` at the site root in **both** dev and
production builds, while files under `src/` are only reachable if something
imports them through the bundler.

The glue script is loaded as a classic (non-module) script from `index.html`:

```html
<script src="/core_engine.js"></script>
```

Build it with:

```sh
make wasm
```

which produces `web/public/core_engine.js` and `web/public/core_engine.wasm`.
Requires `emcmake` / `emcc` in PATH (Emscripten SDK activated).
