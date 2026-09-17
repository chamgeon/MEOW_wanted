import { useCallback, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import type { ReactNode } from 'react';

/**
 * A second browser window that React renders into via a portal.
 *
 * The AI panels outgrew the 480 px sidebar: a diff, a benchmark table and a
 * paragraph of prose are all things you read, and the sidebar is sized for
 * things you glance at. Rendering them through a portal -- rather than
 * re-serialising them into an innerHTML template, as the earlier
 * openOptimizerWindow() helper did -- keeps one copy of the markup, keeps React
 * state updates (job polling, streaming analysis) flowing into the window with
 * no extra plumbing, and keeps the JSX type-checked.
 */

/**
 * The popup must be created inside the click handler itself. Browsers grant
 * window.open() only while transient user activation is live, and a React
 * effect scheduled off a state change is already past it often enough to be
 * blocked -- so this is a hook that opens imperatively, not a component that
 * opens on an `open` prop.
 */
export interface PopupWindowHandle {
  /** Portal target. null whenever the window is closed. */
  mount:   HTMLElement | null;
  isOpen:  boolean;
  /** True once a window.open() call returned null -- i.e. the popup blocker
   *  ate it. Callers use this to fall back to rendering inline. */
  blocked: boolean;
  /** Safe to call when already open: focuses the existing window instead of
   *  tearing down and rebuilding the DOM the portal is attached to. */
  open:    () => boolean;
  close:   () => void;
}

/** Copy the parent document's CSS into the popup.
 *
 *  Dev and prod deliver styles differently -- Vite injects <style> during dev
 *  and emits a <link> in the build -- so both are handled. `node.href` on a
 *  live HTMLLinkElement is already absolute, which matters because the popup's
 *  document URL is about:blank and a relative href would resolve against
 *  nothing. */
function cloneStyles(target: Window): void {
  const head = target.document.head;
  document.querySelectorAll<HTMLElement>('style, link[rel="stylesheet"]').forEach(node => {
    const copy = node.cloneNode(true) as HTMLElement;
    if (node instanceof HTMLLinkElement && copy instanceof HTMLLinkElement) copy.href = node.href;
    head.appendChild(copy);
  });
  const meta = target.document.createElement('meta');
  meta.setAttribute('charset', 'utf-8');
  head.appendChild(meta);
}

export function usePopupWindow(
  name:     string,
  title:    string,
  features = 'width=1040,height=900,menubar=no,toolbar=no,location=no,status=no,resizable=yes,scrollbars=yes',
): PopupWindowHandle {
  const [mount,   setMount]   = useState<HTMLElement | null>(null);
  const [blocked, setBlocked] = useState(false);
  const winRef = useRef<Window | null>(null);

  const close = useCallback(() => {
    const win = winRef.current;
    winRef.current = null;
    setMount(null);
    if (win && !win.closed) win.close();
  }, []);

  const open = useCallback(() => {
    const existing = winRef.current;
    if (existing && !existing.closed) { existing.focus(); return true; }

    const win = window.open('', name, features);
    if (!win) { setBlocked(true); return false; }
    setBlocked(false);

    // A window opened under a name that is already taken is REUSED, not
    // created, so it can arrive carrying the previous run's DOM. Clearing both
    // head and body makes the two cases identical.
    win.document.head.innerHTML = '';
    win.document.body.innerHTML = '';
    win.document.title = title;
    cloneStyles(win);

    win.document.body.className = 'popup-body';
    const root = win.document.createElement('div');
    root.className = 'popup-root';
    win.document.body.appendChild(root);

    winRef.current = win;
    setMount(root);
    win.focus();
    return true;
  }, [name, title, features]);

  // The popup can be closed from its own title bar, which fires nothing we can
  // subscribe to from here -- win.onbeforeunload is unreliable on a document we
  // created rather than navigated to. A 500 ms liveness poll is the portable
  // way to notice, and it only runs while a window is actually open.
  useEffect(() => {
    if (!mount) return;
    const timer = window.setInterval(() => {
      if (winRef.current && winRef.current.closed) {
        winRef.current = null;
        setMount(null);
      }
    }, 500);
    return () => window.clearInterval(timer);
  }, [mount]);

  // Never orphan the window: a reload or an HMR swap of the parent would
  // otherwise leave a detached report window with a dead portal in it.
  useEffect(() => {
    const onUnload = () => { const win = winRef.current; if (win && !win.closed) win.close(); };
    window.addEventListener('beforeunload', onUnload);
    return () => {
      window.removeEventListener('beforeunload', onUnload);
      onUnload();
    };
  }, []);

  return { mount, isOpen: mount !== null, blocked, open, close };
}

/** Renders `children` into the popup, with a title bar and a close button.
 *  Returns null when the window is closed, so callers can mount it
 *  unconditionally. */
export function PopupWindowContent({ handle, heading, subheading, children }: {
  handle:      PopupWindowHandle;
  heading:     string;
  subheading?: ReactNode;
  children:    ReactNode;
}) {
  if (!handle.mount) return null;
  return createPortal(
    <>
      <div className="popup-titlebar">
        <div>
          <h2>{heading}</h2>
          {subheading && <p className="popup-sub">{subheading}</p>}
        </div>
        <button type="button" className="popup-close" onClick={handle.close}>창 닫기</button>
      </div>
      <div className="popup-content">{children}</div>
    </>,
    handle.mount,
  );
}
