// background.ts - MV3 service worker. Owns the native host connection, collects detected
// media per tab, intercepts browser downloads and serves the popup.

import { NativeBridge } from './nativeBridge.js';
import { extractYouTube, matchingAudio, outputContainer } from './youtubeExtractor.js';
import type {
  DetectedMedia,
  HostJob,
  HostSettings,
  MediaKind,
  PageMessage,
  PopupPush,
  PopupRequest,
  PopupState,
  Reply,
  YouTubeExtraction,
} from './types.js';

const bridge = new NativeBridge();

const MAX_MEDIA_PER_TAB = 100;
const MIN_MEDIA_BYTES = 50 * 1024; // ignore tracking pixels and tiny UI sounds
const PERSIST_DELAY_MS = 400;

// ---- state (rebuilt from chrome.storage.session when the service worker wakes up) ----
const mediaByTab = new Map<number, DetectedMedia[]>();
const youtubeByTab = new Map<number, YouTubeExtraction>();
const tabPaths = new Map<number, string>();
const jobs = new Map<string, HostJob>();
const popupPorts = new Set<chrome.runtime.Port>();
let hostSettings: HostSettings | null = null;
let interceptDownloads = true;
let openWindowOnIntercept = true;
let monitorWindowId: number | null = null;
let persistTimer: ReturnType<typeof setTimeout> | null = null;

const ready: Promise<void> = (async () => {
  try {
    const session = await chrome.storage.session.get(['media', 'youtube']);
    const media = session.media as Record<string, DetectedMedia[]> | undefined;
    const youtube = session.youtube as Record<string, YouTubeExtraction> | undefined;
    for (const [tab, list] of Object.entries(media ?? {})) mediaByTab.set(Number(tab), list);
    for (const [tab, value] of Object.entries(youtube ?? {})) youtubeByTab.set(Number(tab), value);
    const local = await chrome.storage.local.get(['interceptDownloads', 'openWindowOnIntercept']);
    interceptDownloads = local.interceptDownloads !== false;
    openWindowOnIntercept = local.openWindowOnIntercept !== false;
  } catch (err) {
    console.warn('IDM Clone: could not restore state', err);
  }
})();

function schedulePersist(): void {
  if (persistTimer) return;
  persistTimer = setTimeout(() => {
    persistTimer = null;
    const media: Record<string, DetectedMedia[]> = {};
    const youtube: Record<string, YouTubeExtraction> = {};
    mediaByTab.forEach((list, tab) => (media[String(tab)] = list));
    youtubeByTab.forEach((value, tab) => (youtube[String(tab)] = value));
    chrome.storage.session.set({ media, youtube }).catch(() => undefined);
  }, PERSIST_DELAY_MS);
}

// ------------------------------------------------------------------ helpers ----

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function fileNameOf(url: string): string {
  try {
    const last = new URL(url).pathname.split('/').filter(Boolean).pop() ?? '';
    return decodeURIComponent(last) || new URL(url).hostname;
  } catch {
    return url.slice(0, 80);
  }
}

function classify(mime: string, url: string): MediaKind | null {
  const type = mime.toLowerCase();
  if (/mpegurl|dash\+xml/.test(type) || /\.(?:m3u8|mpd)(?:[?#]|$)/i.test(url)) return 'stream';
  if (type === 'video/mp2t') return null; // an HLS segment, not a file worth saving
  if (type.startsWith('video/')) return 'video';
  if (type.startsWith('audio/')) return 'audio';
  if (/\.(?:mp4|m4v|webm|mkv|mov|flv|avi)(?:[?#]|$)/i.test(url)) return 'video';
  if (/\.(?:mp3|m4a|aac|ogg|opus|wav)(?:[?#]|$)/i.test(url)) return 'audio';
  return null;
}

function updateBadge(tabId: number): void {
  const count = (mediaByTab.get(tabId)?.length ?? 0) + (youtubeByTab.has(tabId) ? 1 : 0);
  chrome.action.setBadgeText({ tabId, text: count > 0 ? String(count) : '' }).catch(() => undefined);
  chrome.action.setBadgeBackgroundColor({ tabId, color: '#2f81f7' }).catch(() => undefined);
}

function addMedia(tabId: number, item: Omit<DetectedMedia, 'id' | 'detectedAt'>): void {
  if (tabId < 0 || !/^https?:/i.test(item.url) || item.url.length > 4096) return;
  if (/googlevideo\.com\/videoplayback/i.test(item.url)) return; // YouTube chunks: use the YouTube card

  const key = item.url.split('#')[0]!;
  const list = mediaByTab.get(tabId) ?? [];
  const existing = list.find((entry) => entry.url === key);
  if (existing) {
    // The same URL is often seen first by webRequest (no title) and later by the page.
    if (item.title && existing.title === fileNameOf(existing.url)) existing.title = item.title;
    if (item.size > existing.size) existing.size = item.size;
    if (item.pageUrl && !existing.pageUrl) existing.pageUrl = item.pageUrl;
  } else {
    list.push({ ...item, url: key, id: `${tabId}-${Date.now().toString(36)}-${list.length}`, detectedAt: Date.now() });
    if (list.length > MAX_MEDIA_PER_TAB) list.shift();
    mediaByTab.set(tabId, list);
  }
  updateBadge(tabId);
  schedulePersist();
}

async function cookieHeader(url: string): Promise<string> {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    return cookies
      .map((c) => `${c.name}=${c.value}`)
      .join('; ')
      .slice(0, 8000);
  } catch {
    return '';
  }
}

async function browserContext(url: string, pageUrl: string): Promise<{ userAgent: string; referer?: string; cookie?: string }> {
  const cookie = await cookieHeader(url);
  return {
    userAgent: navigator.userAgent,
    ...(pageUrl ? { referer: pageUrl } : {}),
    ...(cookie ? { cookie } : {}),
  };
}

// --------------------------------------------------------------- host events ----

function broadcast(push: PopupPush): void {
  for (const port of popupPorts) {
    try {
      port.postMessage(push);
    } catch {
      popupPorts.delete(port);
    }
  }
}

async function refreshJobs(): Promise<void> {
  try {
    const [list, settings] = await Promise.all([bridge.request('list_downloads', {}), bridge.request('get_settings', {})]);
    jobs.clear();
    list.jobs.forEach((job) => jobs.set(job.id, job));
    hostSettings = settings.settings;
  } catch {
    /* host not ready yet; the 'ready' event triggers another refresh */
  }
}

bridge.onEvent((event) => {
  switch (event.event) {
    case 'ready':
      void refreshJobs().then(() => broadcast(event));
      return;
    case 'progress':
      event.jobs.forEach((job) => jobs.set(job.id, job));
      break;
    case 'state':
      jobs.set(event.job.id, event.job);
      break;
    case 'removed':
      jobs.delete(event.id);
      break;
  }
  broadcast(event);
});

bridge.onStatus((status) => {
  if (!status.connected) jobs.clear();
  broadcast({ event: 'bridge', status });
});

bridge.connect();

// ------------------------------------------------------------ media sniffing ----

function headerValue(headers: chrome.webRequest.HttpHeader[] | undefined, name: string): string {
  const wanted = name.toLowerCase();
  return headers?.find((h) => h.name.toLowerCase() === wanted)?.value ?? '';
}

chrome.webRequest.onResponseStarted.addListener(
  (details) => {
    if (details.tabId < 0 || (details.statusCode !== 200 && details.statusCode !== 206)) return;
    const mime = headerValue(details.responseHeaders, 'content-type').split(';')[0]!.trim();
    const kind = classify(mime, details.url);
    if (!kind) return;

    let size = Number(headerValue(details.responseHeaders, 'content-length')) || 0;
    const range = /\/(\d+)$/.exec(headerValue(details.responseHeaders, 'content-range'));
    if (range) size = Number(range[1]); // for 206 replies Content-Length is only the slice
    if (kind !== 'stream' && size > 0 && size < MIN_MEDIA_BYTES) return;

    void ready.then(() =>
      addMedia(details.tabId, {
        url: details.url,
        kind,
        mime,
        size,
        title: fileNameOf(details.url),
        pageUrl: details.initiator ?? '',
        source: 'webRequest',
      }),
    );
  },
  { urls: ['<all_urls>'], types: ['media', 'xmlhttprequest', 'other', 'object'] },
  ['responseHeaders'],
);

function isPageMessage(value: unknown): value is PageMessage {
  if (typeof value !== 'object' || value === null) return false;
  const type = (value as { type?: unknown }).type;
  return type === 'media-detected' || type === 'youtube-player';
}

async function handlePageMessage(message: PageMessage, tabId: number): Promise<void> {
  await ready;
  if (message.type === 'media-detected') {
    const kind = classify(message.mime, message.url);
    if (!kind) return;
    addMedia(tabId, {
      url: String(message.url),
      kind,
      mime: String(message.mime).slice(0, 100),
      size: 0,
      title: String(message.title || fileNameOf(message.url)).slice(0, 200),
      pageUrl: String(message.pageUrl).slice(0, 2048),
      source: message.via,
    });
    return;
  }
  const extraction = extractYouTube(message.response, String(message.pageUrl).slice(0, 2048));
  if (extraction) {
    youtubeByTab.set(tabId, extraction);
    updateBadge(tabId);
    schedulePersist();
  }
}

chrome.tabs.onRemoved.addListener((tabId) => {
  mediaByTab.delete(tabId);
  youtubeByTab.delete(tabId);
  tabPaths.delete(tabId);
  schedulePersist();
});

chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (!info.url) return;
  // Forget a tab's media on real navigations; ignore #fragment-only changes.
  let path = info.url;
  try {
    const u = new URL(info.url);
    path = u.origin + u.pathname + u.search;
  } catch {
    /* keep raw */
  }
  if (tabPaths.get(tabId) === path) return;
  tabPaths.set(tabId, path);
  mediaByTab.delete(tabId);
  youtubeByTab.delete(tabId);
  updateBadge(tabId);
  schedulePersist();
});

// ------------------------------------------------------- download interception ----

function openMonitorWindow(): void {
  if (!openWindowOnIntercept) return;
  const open = (): void => {
    chrome.windows
      .create({ url: chrome.runtime.getURL('popup.html?window=1'), type: 'popup', width: 460, height: 640 })
      .then((win) => {
        monitorWindowId = win.id ?? null;
      })
      .catch(() => undefined);
  };
  if (monitorWindowId === null) return open();
  chrome.windows
    .get(monitorWindowId)
    .catch(() => {
      monitorWindowId = null;
      open();
    });
}

chrome.windows.onRemoved.addListener((id) => {
  if (id === monitorWindowId) monitorWindowId = null;
});

chrome.downloads.onCreated.addListener((item) => {
  void (async () => {
    await ready;
    if (!interceptDownloads || item.byExtensionId) return; // another extension's download: leave it
    const url = item.finalUrl || item.url;
    if (!/^https?:\/\//i.test(url)) return; // blob:, data:, file: cannot be fetched by the host
    if (!bridge.isConnected()) return; // fail-safe: if the host is down, Chrome downloads normally

    try {
      const context = await browserContext(url, item.referrer);
      const baseName = item.filename ? item.filename.split(/[\\/]/).pop() : '';
      await bridge.request('start_download', { url, ...context, ...(baseName ? { fileName: baseName } : {}) });
      // Only now that the host has accepted the job do we cancel Chrome's own copy.
      await chrome.downloads.cancel(item.id).catch(() => undefined);
      await chrome.downloads.erase({ id: item.id }).catch(() => undefined);
      openMonitorWindow();
    } catch (err) {
      console.warn('IDM Clone: host refused the download, leaving it to Chrome:', errorMessage(err));
    }
  })();
});

const MENU_ID = 'idmclone-download';

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: MENU_ID,
      title: 'Download with IDM Clone',
      contexts: ['link', 'video', 'audio'],
    });
  });
});

chrome.contextMenus.onClicked.addListener((info) => {
  if (info.menuItemId !== MENU_ID) return;
  const url = info.linkUrl ?? info.srcUrl;
  if (!url || !/^https?:\/\//i.test(url)) return;
  void (async () => {
    try {
      const context = await browserContext(url, info.pageUrl ?? '');
      await bridge.request('start_download', { url, ...context });
      openMonitorWindow();
    } catch (err) {
      console.warn('IDM Clone: context-menu download failed:', errorMessage(err));
    }
  })();
});

// ------------------------------------------------------------ popup requests ----

function ok<T>(data: T): Reply<T> {
  return { ok: true, data };
}

async function getState(tabId: number): Promise<PopupState> {
  if (bridge.isConnected() && (jobs.size === 0 || hostSettings === null)) await refreshJobs();
  return {
    host: bridge.status,
    media: mediaByTab.get(tabId) ?? [],
    youtube: youtubeByTab.get(tabId) ?? null,
    jobs: [...jobs.values()].sort((a, b) => b.createdAt - a.createdAt),
    settings: hostSettings,
    interceptDownloads,
    openWindowOnIntercept,
  };
}

async function downloadMedia(tabId: number, mediaId: string): Promise<{ jobId: string }> {
  const item = mediaByTab.get(tabId)?.find((entry) => entry.id === mediaId);
  if (!item) throw new Error('That item is no longer available. Reload the page.');
  const context = await browserContext(item.url, item.pageUrl);
  if (item.kind === 'stream') {
    // HLS/DASH manifests are handled by yt-dlp (it downloads, joins and muxes the pieces).
    const { cookie: _cookie, ...safe } = context; // a manifest's CDNs must not receive site cookies
    void _cookie;
    return bridge.request('start_ytdlp_download', { url: item.url, title: item.title, ...safe });
  }
  return bridge.request('start_download', { url: item.url, title: item.title, ...context });
}

async function downloadYouTubeMerge(tabId: number, videoItag: number, audioItag: number): Promise<{ jobId: string }> {
  const extraction = youtubeByTab.get(tabId);
  if (!extraction) throw new Error('No YouTube data for this tab. Reload the video page.');
  const video = extraction.videos.find((v) => v.itag === videoItag);
  const audio = extraction.audios.find((a) => a.itag === audioItag) ?? (video ? matchingAudio(extraction, video) : null);
  if (!video || !audio) throw new Error('Selected format not found');
  if (!video.url || !audio.url) {
    throw new Error('These streams are protected by YouTube. Use "Download with yt-dlp" instead.');
  }
  return bridge.request('start_merge_download', {
    videoUrl: video.url,
    audioUrl: audio.url,
    title: extraction.title,
    outputExt: outputContainer(video),
    userAgent: navigator.userAgent,
    referer: 'https://www.youtube.com/',
  });
}

async function handlePopupRequest(request: PopupRequest): Promise<Reply<unknown>> {
  await ready;
  try {
    switch (request.type) {
      case 'get-state':
        return ok(await getState(request.tabId));
      case 'download-media':
        return ok(await downloadMedia(request.tabId, request.mediaId));
      case 'download-youtube-merge':
        return ok(await downloadYouTubeMerge(request.tabId, request.videoItag, request.audioItag));
      case 'download-ytdlp': {
        const { cookie: _cookie, ...context } = await browserContext(request.url, request.pageUrl ?? '');
        void _cookie; // yt-dlp sends headers to every host it contacts; use "cookies from browser" for logins
        return ok(
          await bridge.request('start_ytdlp_download', {
            url: request.url,
            ...(request.title ? { title: request.title } : {}),
            ...(request.formatSelector ? { formatSelector: request.formatSelector } : {}),
            ...context,
          }),
        );
      }
      case 'ytdlp-formats':
        return ok((await bridge.request('ytdlp_formats', { url: request.url, userAgent: navigator.userAgent }, 120000)).info);
      case 'job-action':
        if (request.action === 'open_folder') return ok(await bridge.request('open_folder', { jobId: request.jobId }));
        return ok(await bridge.request(request.action, { jobId: request.jobId }));
      case 'pause-all':
        return ok(await bridge.request('pause_all', {}));
      case 'set-settings': {
        const result = await bridge.request('set_settings', { settings: request.settings });
        hostSettings = result.settings;
        return ok(result.settings);
      }
      case 'set-intercept':
        interceptDownloads = request.enabled;
        openWindowOnIntercept = request.openWindow;
        await chrome.storage.local.set({ interceptDownloads, openWindowOnIntercept });
        return ok(null);
      case 'clear-media':
        mediaByTab.delete(request.tabId);
        updateBadge(request.tabId);
        schedulePersist();
        return ok(null);
      case 'reconnect':
        bridge.reconnectNow();
        return ok(null);
    }
  } catch (err) {
    return { ok: false, error: errorMessage(err) };
  }
}

/** True for our own pages (popup, monitor window). Content scripts report the web page's URL instead. */
function isExtensionPage(sender: chrome.runtime.MessageSender): boolean {
  return typeof sender.url === 'string' && sender.url.startsWith(chrome.runtime.getURL(''));
}

chrome.runtime.onMessage.addListener((message: unknown, sender, sendResponse) => {
  if (sender.id !== chrome.runtime.id) return false;
  if (typeof message !== 'object' || message === null || !('type' in message)) return false;

  if (isPageMessage(message)) {
    // Page reports come only from content scripts running in tabs.
    if (!isExtensionPage(sender) && sender.tab?.id !== undefined) void handlePageMessage(message, sender.tab.id);
    return false;
  }
  // Commands (download, settings, ...) are accepted only from our own pages. Note that the
  // monitor window is a tab too, so "has sender.tab" cannot be used to spot content scripts.
  if (!isExtensionPage(sender)) return false;
  void handlePopupRequest(message as PopupRequest).then(sendResponse);
  return true; // keep the channel open for the async response
});

chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== 'popup' || port.sender?.id !== chrome.runtime.id) return;
  popupPorts.add(port);
  port.onDisconnect.addListener(() => popupPorts.delete(port));
});
