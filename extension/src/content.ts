// content.ts - media sniffer. Runs in the PAGE's own JavaScript world.
//
// manifest.json registers this file with  "world": "MAIN"  (Chrome 111+). That is
// required for two reasons:
//   1. patching window.fetch / XMLHttpRequest only affects the page if we patch the
//      page's copies, not the isolated-world copies;
//   2. window.ytInitialPlayerResponse is a page variable invisible to isolated scripts.
// A MAIN-world script has no chrome.* APIs, so it hands findings to relay.ts (isolated
// world) with window.postMessage, and relay.ts forwards them to the service worker.
//
// This file must stay a classic script: no import/export statements.

interface Window {
  ytInitialPlayerResponse?: unknown;
  __idmcloneInstalled?: boolean;
}

(() => {
  if (window.__idmcloneInstalled) return; // all_frames + about:blank can inject twice
  window.__idmcloneInstalled = true;

  const SOURCE = 'idmclone-page';
  const MEDIA_URL = /\.(mp4|m4v|m4a|webm|mkv|mov|flv|avi|mp3|aac|ogg|opus|wav|m3u8|mpd)(?:[?#]|$)/i;
  const MEDIA_MIME = /^(?:video|audio)\/|mpegurl|dash\+xml/i;
  // YouTube's per-chunk media requests are handled by the dedicated YouTube path below.
  const NOISE = /googlevideo\.com\/videoplayback/i;
  const seen = new Set<string>();

  function post(message: Record<string, unknown>): void {
    try {
      window.postMessage({ source: SOURCE, ...message }, '*');
    } catch {
      /* page revoked postMessage or payload not cloneable: ignore */
    }
  }

  function toAbsolute(raw: string): string {
    try {
      return new URL(raw, location.href).href;
    } catch {
      return '';
    }
  }

  function report(rawUrl: string, via: 'dom' | 'fetch' | 'xhr', mime = ''): void {
    const url = toAbsolute(rawUrl);
    // blob: and data: URLs only exist inside this browser process; another process cannot fetch them.
    if (!/^https?:/i.test(url) || NOISE.test(url)) return;
    if (seen.has(url)) return;
    if (!MEDIA_URL.test(url) && !MEDIA_MIME.test(mime)) return;
    seen.add(url);
    post({ type: 'media-detected', url, via, mime, pageUrl: location.href, title: document.title });
  }

  // ---- fetch ---------------------------------------------------------------
  const nativeFetch = window.fetch;
  window.fetch = function patchedFetch(this: unknown, input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    const result: Promise<Response> = nativeFetch.call(this, input, init);
    try {
      const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
      report(url, 'fetch');
      // Observe the response without disturbing the page's own copy of it.
      void result
        .then((response) => {
          const type = response.headers.get('content-type') ?? '';
          if (MEDIA_MIME.test(type)) report(url, 'fetch', type);
          if (url.includes('/youtubei/v1/player')) {
            return response.clone().json() as Promise<unknown>;
          }
          return undefined;
        })
        .then((json) => {
          if (json !== undefined) sendYouTube(json);
        })
        .catch(() => undefined);
    } catch {
      /* never let the sniffer break the page's request */
    }
    return result;
  };

  // ---- XMLHttpRequest -------------------------------------------------------
  const nativeOpen = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function patchedOpen(this: XMLHttpRequest, ...args: unknown[]): void {
    try {
      const url = String(args[1]);
      report(url, 'xhr');
      this.addEventListener('readystatechange', () => {
        if (this.readyState === XMLHttpRequest.HEADERS_RECEIVED) {
          const type = this.getResponseHeader('content-type') ?? '';
          if (MEDIA_MIME.test(type)) report(url, 'xhr', type);
        }
      });
    } catch {
      /* ignore */
    }
    Reflect.apply(nativeOpen, this, args);
  } as typeof XMLHttpRequest.prototype.open;

  // ---- <video>, <audio>, <source> -------------------------------------------
  function scanElement(el: Element): void {
    if (el instanceof HTMLMediaElement) {
      const src = el.currentSrc || el.src;
      if (src) report(src, 'dom', el instanceof HTMLVideoElement ? 'video/*' : 'audio/*');
    } else if (el instanceof HTMLSourceElement && el.src) {
      report(el.src, 'dom', el.type || 'video/*');
    }
  }

  function scanTree(root: ParentNode): void {
    root.querySelectorAll('video, audio, source').forEach(scanElement);
  }

  function startObserving(): void {
    scanTree(document);
    new MutationObserver((records) => {
      for (const record of records) {
        if (record.type === 'attributes' && record.target instanceof Element) scanElement(record.target);
        record.addedNodes.forEach((node) => {
          if (node instanceof Element) {
            scanElement(node);
            scanTree(node);
          }
        });
      }
    }).observe(document.documentElement, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['src'],
    });
  }

  if (document.documentElement) startObserving();
  else document.addEventListener('DOMContentLoaded', startObserving, { once: true });

  // currentSrc is only known after the browser picked a <source>. Media events do not bubble, so capture.
  document.addEventListener(
    'loadedmetadata',
    (event) => {
      if (event.target instanceof Element) scanElement(event.target);
    },
    true,
  );

  // ---- YouTube ----------------------------------------------------------------
  const isYouTube = /(^|\.)youtube\.com$/i.test(location.hostname);
  let lastSignature = '';

  function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null;
  }

  function sendYouTube(response: unknown): void {
    if (!isYouTube || !isRecord(response)) return;
    const details = isRecord(response.videoDetails) ? response.videoDetails : null;
    const streaming = isRecord(response.streamingData) ? response.streamingData : null;
    if (!details && !streaming) return;

    // After an in-page (SPA) navigation ytInitialPlayerResponse still describes the previous video.
    const videoId = typeof details?.videoId === 'string' ? details.videoId : '';
    const urlVideoId = new URLSearchParams(location.search).get('v');
    if (urlVideoId && videoId && urlVideoId !== videoId) return;

    const adaptive = streaming && Array.isArray(streaming.adaptiveFormats) ? streaming.adaptiveFormats.length : 0;
    const signature = `${videoId}:${adaptive}`;
    if (signature === lastSignature) return;
    lastSignature = signature;

    try {
      // Round-trip through JSON: guarantees structured-clone-able plain data and drops the rest.
      const trimmed: unknown = JSON.parse(
        JSON.stringify({
          videoDetails: response.videoDetails,
          streamingData: response.streamingData,
          playabilityStatus: response.playabilityStatus,
        }),
      );
      post({ type: 'youtube-player', response: trimmed, pageUrl: location.href });
    } catch {
      /* ignore */
    }
  }

  function readYouTubePlayer(): void {
    // movie_player.getPlayerResponse() tracks the current video even after SPA navigation.
    const player = document.getElementById('movie_player') as (HTMLElement & { getPlayerResponse?: () => unknown }) | null;
    let response: unknown;
    try {
      response = player?.getPlayerResponse?.();
    } catch {
      response = undefined;
    }
    sendYouTube(response ?? window.ytInitialPlayerResponse);
  }

  if (isYouTube) {
    document.addEventListener('yt-navigate-finish', () => setTimeout(readYouTubePlayer, 800));
    window.addEventListener('load', () => setTimeout(readYouTubePlayer, 500));
    setInterval(readYouTubePlayer, 4000); // cheap: a property read + signature compare
  }
})();
