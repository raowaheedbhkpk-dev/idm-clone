// youtubeExtractor.ts - turns a YouTube player response into video-only / audio-only DASH formats.
//
// What this module does NOT do: decipher `signatureCipher` or the throttling "n"
// parameter. Those algorithms change every few weeks; yt-dlp tracks them. Streams
// that need deciphering are returned with needsCipher = true and the UI routes
// them to yt-dlp (the native host runs it as a subprocess).

import type { StreamFormat, YouTubeExtraction } from './types.js';

/** Well-known itags. Sources: public itag tables; only used for display. */
export const ITAG_INFO: Readonly<Record<number, string>> = {
  // muxed (audio+video in one file) - legacy
  18: '360p mp4 (muxed)',
  22: '720p mp4 (muxed)',
  // video only, H.264 (avc1) in mp4
  160: '144p H.264',
  133: '240p H.264',
  134: '360p H.264',
  135: '480p H.264',
  136: '720p H.264',
  137: '1080p H.264',
  298: '720p60 H.264',
  299: '1080p60 H.264',
  264: '1440p H.264',
  266: '2160p H.264',
  // video only, VP9 in webm
  278: '144p VP9',
  242: '240p VP9',
  243: '360p VP9',
  244: '480p VP9',
  247: '720p VP9',
  248: '1080p VP9',
  271: '1440p VP9',
  313: '2160p VP9',
  302: '720p60 VP9',
  303: '1080p60 VP9',
  308: '1440p60 VP9',
  315: '2160p60 VP9',
  // video only, AV1 in mp4
  394: '144p AV1',
  395: '240p AV1',
  396: '360p AV1',
  397: '480p AV1',
  398: '720p AV1',
  399: '1080p AV1',
  400: '1440p AV1',
  401: '2160p AV1',
  // audio only
  139: '48 kbps HE-AAC (m4a)',
  140: '128 kbps AAC (m4a)',
  141: '256 kbps AAC (m4a)',
  249: '50 kbps Opus (webm)',
  250: '70 kbps Opus (webm)',
  251: '160 kbps Opus (webm)',
};

type UnknownRecord = Record<string, unknown>;

function isRecord(value: unknown): value is UnknownRecord {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function str(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function num(value: unknown): number {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string') {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return 0;
}

export interface ParsedMime {
  type: 'video' | 'audio' | 'other';
  container: string;
  codecs: string;
}

/** `video/mp4; codecs="avc1.64001F"` -> { type: 'video', container: 'mp4', codecs: 'avc1.64001F' } */
export function parseMime(mime: string): ParsedMime {
  const match = /^(video|audio)\/([a-z0-9.+-]+)\s*(?:;\s*codecs="([^"]*)")?/i.exec(mime);
  if (!match) return { type: 'other', container: '', codecs: '' };
  const type = match[1]!.toLowerCase() === 'video' ? 'video' : 'audio';
  let container = match[2]!.toLowerCase();
  if (type === 'audio' && container === 'mp4') container = 'm4a'; // audio/mp4 is an .m4a file
  return { type, container, codecs: match[3] ?? '' };
}

function readFormat(raw: unknown): StreamFormat | null {
  if (!isRecord(raw)) return null;
  const itag = num(raw.itag);
  const mimeType = str(raw.mimeType);
  const mime = parseMime(mimeType);
  if (itag <= 0 || mime.type === 'other') return null;

  const directUrl = str(raw.url);
  const cipher = str(raw.signatureCipher) || str(raw.cipher);
  const height = num(raw.height);
  const bitrate = num(raw.averageBitrate) || num(raw.bitrate);

  const format: StreamFormat = {
    itag,
    kind: mime.type,
    container: mime.container,
    mimeType,
    codecs: mime.codecs,
    qualityLabel:
      str(raw.qualityLabel) ||
      (mime.type === 'audio' ? `${Math.round(bitrate / 1000)} kbps` : height ? `${height}p` : ''),
    width: num(raw.width),
    height,
    fps: num(raw.fps),
    bitrate,
    contentLength: num(raw.contentLength),
    description: ITAG_INFO[itag] ?? '',
    needsCipher: directUrl === '' && cipher !== '',
  };
  if (directUrl !== '') format.url = directUrl;
  return format;
}

/** Why a player response cannot be used, or '' when it looks fine. */
export function describeProblem(response: UnknownRecord, pageUrl: string): string {
  // EU/UK users are redirected to a consent wall before any player data exists.
  // We deliberately do not forge consent cookies; the user must accept it once.
  if (/^https?:\/\/consent\.youtube\.com\//i.test(pageUrl)) {
    return 'YouTube is showing its cookie-consent page. Accept it, then reopen this popup.';
  }
  const playability = response.playabilityStatus;
  if (isRecord(playability)) {
    const status = str(playability.status);
    if (status !== '' && status !== 'OK') {
      const reason = str(playability.reason) || status;
      if (status === 'LOGIN_REQUIRED') return `Sign-in required (${reason}). Sign in to YouTube in this browser.`;
      return `${reason} (${status})`;
    }
  }
  return '';
}

/**
 * Parses (the useful subset of) ytInitialPlayerResponse / the /youtubei/v1/player reply.
 * Returns null when `response` does not look like a player response at all.
 */
export function extractYouTube(response: unknown, pageUrl: string): YouTubeExtraction | null {
  if (!isRecord(response)) return null;
  const details = isRecord(response.videoDetails) ? response.videoDetails : null;
  const streaming = isRecord(response.streamingData) ? response.streamingData : null;
  if (!details && !streaming) return null;

  const result: YouTubeExtraction = {
    videoId: str(details?.videoId),
    title: str(details?.title) || 'YouTube video',
    author: str(details?.author),
    lengthSeconds: num(details?.lengthSeconds),
    pageUrl,
    videos: [],
    audios: [],
    problem: describeProblem(response, pageUrl),
  };

  const adaptive = streaming && Array.isArray(streaming.adaptiveFormats) ? streaming.adaptiveFormats : [];
  for (const raw of adaptive) {
    const format = readFormat(raw);
    if (!format) continue;
    (format.kind === 'video' ? result.videos : result.audios).push(format);
  }

  // Best first: height, then fps, then bitrate (videos); bitrate (audios).
  result.videos.sort((a, b) => b.height - a.height || b.fps - a.fps || b.bitrate - a.bitrate);
  result.audios.sort((a, b) => b.bitrate - a.bitrate);

  if (result.problem === '' && result.videos.length === 0 && result.audios.length === 0) {
    result.problem = 'No downloadable streams in the player response (live stream or protected video).';
  }
  return result;
}

export interface FormatPair {
  video: StreamFormat;
  audio: StreamFormat;
}

/** Audio container that can be stream-copied into the same container as the video. */
function compatibleAudioContainer(videoContainer: string): string {
  return videoContainer === 'webm' ? 'webm' : 'm4a';
}

/**
 * Best video+audio combination that has direct URLs and can be muxed without
 * re-encoding: mp4 video + m4a audio, or webm video + webm audio.
 * mp4 is preferred because it plays everywhere.
 */
export function pickBestPair(extraction: YouTubeExtraction, maxHeight = Number.POSITIVE_INFINITY): FormatPair | null {
  for (const container of ['mp4', 'webm']) {
    const video = extraction.videos.find((v) => v.container === container && v.url && v.height <= maxHeight);
    const audio = extraction.audios.find((a) => a.container === compatibleAudioContainer(container) && a.url);
    if (video && audio) return { video, audio };
  }
  return null;
}

/** Finds an audio stream that can be muxed with the chosen video stream. */
export function matchingAudio(extraction: YouTubeExtraction, video: StreamFormat): StreamFormat | null {
  const wanted = compatibleAudioContainer(video.container);
  return extraction.audios.find((a) => a.container === wanted && a.url) ?? null;
}

/** Output container for stream-copying `video` (+ its matching audio). */
export function outputContainer(video: StreamFormat): string {
  return video.container === 'webm' ? 'webm' : 'mp4';
}

export function describeFormat(format: StreamFormat): string {
  const parts: string[] = [];
  parts.push(format.kind === 'video' ? format.qualityLabel || `${format.height}p` : format.qualityLabel);
  parts.push(format.container);
  const codec = format.codecs.split('.')[0];
  if (codec) parts.push(codec);
  if (format.contentLength > 0) parts.push(`${(format.contentLength / 1048576).toFixed(1)} MB`);
  parts.push(format.kind === 'video' ? 'video only' : 'audio only');
  if (format.needsCipher) parts.push('needs yt-dlp');
  return parts.filter(Boolean).join(' · ');
}
