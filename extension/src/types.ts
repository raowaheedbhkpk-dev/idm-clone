// types.ts - shared types: host protocol, detected media, popup <-> background messages.

// ------------------------------------------------------------ host side ----

export type JobState = 'QUEUED' | 'DOWNLOADING' | 'PAUSED' | 'MERGING' | 'COMPLETED' | 'FAILED';
export type JobKind = 'direct' | 'merge' | 'ytdlp';

export interface HostJob {
  id: string;
  kind: JobKind;
  state: JobState;
  url: string;
  title: string;
  fileName: string;
  folder: string;
  finalPath: string;
  downloaded: number;
  total: number;
  percent: number;
  speed: number; // bytes/s
  eta: number; // seconds, -1 = unknown
  error: string;
  segments: number;
  createdAt: number;
}

export interface HostSettings {
  maxConcurrent: number;
  segments: number;
  defaultFolder: string;
  cookiesFromBrowser: string;
}

/** Optional request context that makes the host download like the browser would. */
export interface RequestContext {
  userAgent?: string;
  referer?: string;
  cookie?: string;
  headers?: Record<string, string>;
  folder?: string;
  segments?: number;
}

export interface YtFormat {
  formatId: string;
  ext: string;
  resolution: string;
  vcodec: string;
  acodec: string;
  note: string;
  filesize: number;
  height: number;
  fps: number;
  tbr: number;
  url?: string;
  httpHeaders?: Record<string, string>;
}

export interface YtInfo {
  id: string;
  title: string;
  thumbnail: string;
  uploader: string;
  webpageUrl: string;
  duration: number;
  formats: YtFormat[];
}

type NoPayload = Record<string, never>;
interface JobIdPayload {
  jobId: string;
}

/** command name -> request payload / response payload (the host adds `ok` and `id`). */
export interface CommandMap {
  ping: {
    req: NoPayload;
    res: { version: string; ffmpeg: boolean; ytdlp: boolean; settings: HostSettings };
  };
  start_download: {
    req: RequestContext & { url: string; fileName?: string; title?: string };
    res: { jobId: string };
  };
  start_merge_download: {
    req: RequestContext & { videoUrl: string; audioUrl: string; title: string; outputExt: string };
    res: { jobId: string };
  };
  start_ytdlp_download: {
    req: RequestContext & { url: string; title?: string; formatSelector?: string; outputExt?: string };
    res: { jobId: string };
  };
  ytdlp_formats: {
    req: { url: string; referer?: string; userAgent?: string };
    res: { info: YtInfo };
  };
  pause: { req: JobIdPayload; res: NoPayload };
  resume: { req: JobIdPayload; res: NoPayload };
  cancel: { req: JobIdPayload; res: NoPayload };
  remove: { req: JobIdPayload; res: NoPayload };
  pause_all: { req: NoPayload; res: NoPayload };
  get_status: { req: JobIdPayload; res: { job: HostJob } };
  list_downloads: { req: NoPayload; res: { jobs: HostJob[] } };
  get_settings: { req: NoPayload; res: { settings: HostSettings } };
  set_settings: { req: { settings: Partial<HostSettings> }; res: { settings: HostSettings } };
  open_folder: { req: { jobId?: string }; res: NoPayload };
  shutdown: { req: NoPayload; res: NoPayload };
}
export type Command = keyof CommandMap;

export type HostEvent =
  | { event: 'ready'; version: string }
  | { event: 'progress'; jobs: HostJob[] }
  | { event: 'state'; job: HostJob }
  | { event: 'removed'; id: string };

export interface BridgeStatus {
  connected: boolean;
  lastError: string;
  hostVersion: string;
  ffmpeg: boolean;
  ytdlp: boolean;
}

// ---------------------------------------------------------- media sniffing ----

export type MediaKind = 'video' | 'audio' | 'stream' | 'file';
export type MediaSource = 'webRequest' | 'dom' | 'fetch' | 'xhr';

export interface DetectedMedia {
  id: string;
  url: string;
  kind: MediaKind;
  mime: string;
  size: number; // bytes, 0 = unknown
  title: string;
  pageUrl: string;
  source: MediaSource;
  detectedAt: number;
}

// ---------------------------------------------------------------- YouTube ----

export interface StreamFormat {
  itag: number;
  kind: 'video' | 'audio';
  container: string; // mp4, webm, m4a ...
  mimeType: string;
  codecs: string;
  qualityLabel: string; // "1080p60" or "128 kbps"
  width: number;
  height: number;
  fps: number;
  bitrate: number; // bits/s
  contentLength: number; // bytes, 0 = unknown
  description: string; // from the itag table, may be empty
  /** Direct URL, present only when the player did not cipher it. */
  url?: string;
  /** True when the stream is protected by signatureCipher; only yt-dlp can fetch it. */
  needsCipher: boolean;
}

export interface YouTubeExtraction {
  videoId: string;
  title: string;
  author: string;
  lengthSeconds: number;
  pageUrl: string;
  videos: StreamFormat[];
  audios: StreamFormat[];
  /** Non-empty when the video cannot be played/extracted (login, age gate, consent, ...). */
  problem: string;
}

// ------------------------------------------------- popup <-> background ----

export interface PopupState {
  host: BridgeStatus;
  media: DetectedMedia[];
  youtube: YouTubeExtraction | null;
  jobs: HostJob[];
  settings: HostSettings | null;
  interceptDownloads: boolean;
  openWindowOnIntercept: boolean;
}

/** Everything the background pushes to an open popup over its long-lived port. */
export type PopupPush = HostEvent | { event: 'bridge'; status: BridgeStatus };

export type PopupRequest =
  | { type: 'get-state'; tabId: number }
  | { type: 'download-media'; tabId: number; mediaId: string }
  | { type: 'download-youtube-merge'; tabId: number; videoItag: number; audioItag: number }
  | { type: 'download-ytdlp'; url: string; title?: string; formatSelector?: string; pageUrl?: string }
  | { type: 'ytdlp-formats'; url: string }
  | { type: 'job-action'; action: 'pause' | 'resume' | 'cancel' | 'remove' | 'open_folder'; jobId: string }
  | { type: 'pause-all' }
  | { type: 'set-settings'; settings: Partial<HostSettings> }
  | { type: 'set-intercept'; enabled: boolean; openWindow: boolean }
  | { type: 'clear-media'; tabId: number }
  | { type: 'reconnect' };

export type Reply<T> = { ok: true; data: T } | { ok: false; error: string };

/** Messages the relay content script sends to the background. */
export type PageMessage =
  | {
      type: 'media-detected';
      url: string;
      via: 'dom' | 'fetch' | 'xhr';
      mime: string;
      pageUrl: string;
      title: string;
    }
  | { type: 'youtube-player'; response: unknown; pageUrl: string };
