// popup.ts - IDM-style UI. All text from web pages (titles, URLs) is inserted with
// textContent, never innerHTML, so a hostile page cannot inject markup into the popup.

import { describeFormat, matchingAudio, pickBestPair } from './youtubeExtractor.js';
import type {
  BridgeStatus,
  DetectedMedia,
  HostJob,
  PopupPush,
  PopupRequest,
  PopupState,
  Reply,
  YouTubeExtraction,
  YtFormat,
  YtInfo,
} from './types.js';

// ------------------------------------------------------------------ plumbing ----

const windowMode = new URLSearchParams(location.search).get('window') === '1';
if (windowMode) document.body.classList.add('window-mode');

function $<T extends HTMLElement = HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`#${id} not found in popup.html`);
  return node as T;
}

type Child = Node | string | null | undefined | false;
interface Props {
  className?: string;
  text?: string;
  title?: string;
  type?: string;
  disabled?: boolean;
  hidden?: boolean;
  value?: string;
  selected?: boolean;
  onclick?: () => void;
}

function el<K extends keyof HTMLElementTagNameMap>(tag: K, props: Props = {}, ...children: Child[]): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (props.className) node.className = props.className;
  if (props.text !== undefined) node.textContent = props.text;
  if (props.title) node.title = props.title;
  if (props.type) node.setAttribute('type', props.type);
  if (props.disabled !== undefined) (node as unknown as { disabled: boolean }).disabled = props.disabled;
  if (props.hidden !== undefined) node.hidden = props.hidden;
  if (props.value !== undefined) (node as unknown as { value: string }).value = props.value;
  if (props.selected !== undefined) (node as unknown as { selected: boolean }).selected = props.selected;
  if (props.onclick) node.addEventListener('click', props.onclick);
  for (const child of children) {
    if (child) node.append(child);
  }
  return node;
}

async function send<T>(request: PopupRequest): Promise<T> {
  const reply = (await chrome.runtime.sendMessage(request)) as Reply<T> | undefined;
  if (!reply) throw new Error('The extension did not answer. Reload it in chrome://extensions.');
  if (!reply.ok) throw new Error(reply.error);
  return reply.data;
}

let toastTimer: ReturnType<typeof setTimeout> | undefined;
function toast(message: string, isError = false): void {
  const node = $('toast');
  node.textContent = message;
  node.className = `show${isError ? ' error' : ''}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (node.className = ''), isError ? 5000 : 2200);
}

function fail(err: unknown): void {
  toast(err instanceof Error ? err.message : String(err), true);
}

function formatBytes(bytes: number): string {
  if (!(bytes > 0)) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value >= 100 || unit === 0 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

function formatDuration(seconds: number): string {
  if (!(seconds >= 0) || seconds > 10 * 86400) return '--';
  const s = Math.round(seconds);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${String(m).padStart(2, '0')}m`;
  return m > 0 ? `${m}m ${String(sec).padStart(2, '0')}s` : `${sec}s`;
}

function hostOf(url: string): string {
  try {
    return new URL(url).hostname;
  } catch {
    return '';
  }
}

// --------------------------------------------------------------------- state ----

let tabId = -1;
let tabUrl = '';
let tabTitle = '';
let host: BridgeStatus = { connected: false, lastError: '', hostVersion: '', ffmpeg: false, ytdlp: false };
let currentState: PopupState | null = null;
const jobs = new Map<string, HostJob>();

// ---------------------------------------------------------------------- tabs ----

function activateTab(name: string): void {
  document.querySelectorAll<HTMLButtonElement>('.tabs button').forEach((button) => {
    button.classList.toggle('active', button.dataset.tab === name);
  });
  document.querySelectorAll<HTMLElement>('.panel').forEach((panel) => {
    panel.classList.toggle('active', panel.id === `tab-${name}`);
  });
}

document.querySelectorAll<HTMLButtonElement>('.tabs button').forEach((button) => {
  button.addEventListener('click', () => activateTab(button.dataset.tab ?? 'media'));
});

// ---------------------------------------------------------------- host status ----

function renderHost(status: BridgeStatus): void {
  host = status;
  $('hostDot').classList.toggle('on', status.connected);
  let text: string;
  if (status.connected) {
    text = `Host v${status.hostVersion || '?'} connected`;
  } else if (/not found|forbidden/i.test(status.lastError)) {
    text = 'Native host not installed (see README)';
  } else {
    text = status.lastError ? `Disconnected: ${status.lastError}` : 'Connecting...';
  }
  const hostText = $('hostText');
  hostText.textContent = text;
  hostText.title = status.lastError || text;
  $('reconnect').hidden = status.connected;
  $('hostInfo').textContent = status.connected
    ? `ffmpeg.exe: ${status.ffmpeg ? 'found' : 'MISSING (needed to merge video + audio)'}  |  yt-dlp.exe: ${
        status.ytdlp ? 'found' : 'MISSING (needed for sites, HLS/DASH and protected YouTube)'
      }`
    : 'Install the native host first: run installer\\register_host.reg and copy idm-clone-host.exe to C:\\Program Files\\IDMClone.';
}

$('reconnect').addEventListener('click', () => {
  send<null>({ type: 'reconnect' }).catch(fail);
});

// --------------------------------------------------------------------- media ----

let lastMediaSignature = '';

function mediaRow(item: DetectedMedia): HTMLElement {
  const sizeText = item.size > 0 ? ` · ${formatBytes(item.size)}` : '';
  return el(
    'div',
    { className: 'card media' },
    el('span', { className: 'badge', text: item.kind }),
    el(
      'div',
      { className: 'info' },
      el('div', { className: 'name', text: item.title || item.url, title: item.url }),
      el('div', { className: 'sub', text: `${hostOf(item.url)}${sizeText}` }),
    ),
    el('button', {
      className: 'primary',
      text: 'Download',
      title: item.kind === 'stream' ? 'Streams are downloaded and merged by yt-dlp' : 'Download with multiple connections',
      disabled: item.kind === 'stream' && !host.ytdlp,
      onclick: () => {
        send<{ jobId: string }>({ type: 'download-media', tabId, mediaId: item.id })
          .then(() => toast('Added to downloads'))
          .catch(fail);
      },
    }),
  );
}

function renderMedia(media: DetectedMedia[]): void {
  $('mediaCount').textContent = String(media.length);
  const signature = media.map((m) => `${m.id}|${m.title}|${m.size}`).join(';') + `|${host.ytdlp}`;
  if (signature !== lastMediaSignature) {
    lastMediaSignature = signature;
    $('mediaList').replaceChildren(...[...media].reverse().map(mediaRow));
  }
  $('mediaTitle').hidden = media.length === 0;
  $('mediaEmpty').hidden = media.length > 0 || currentState?.youtube != null;
}

$('clearMedia').addEventListener('click', () => {
  send<null>({ type: 'clear-media', tabId })
    .then(() => {
      lastMediaSignature = '';
      return refresh();
    })
    .catch(fail);
});

// ------------------------------------------------------------------- YouTube ----

let lastYouTubeSignature = '';

function renderYouTube(yt: YouTubeExtraction | null): void {
  const signature = yt ? `${yt.videoId}|${yt.videos.length}|${yt.audios.length}|${yt.problem}|${host.ffmpeg}` : '';
  if (signature === lastYouTubeSignature) return;
  lastYouTubeSignature = signature;
  const box = $('youtube');
  box.replaceChildren();
  if (!yt) return;

  const card = el(
    'div',
    { className: 'card' },
    el('h3', { text: yt.title, title: yt.title }),
    el('div', { className: 'sub', text: `YouTube · ${yt.author || 'unknown channel'} · ${formatDuration(yt.lengthSeconds)}` }),
  );
  if (yt.problem) card.append(el('div', { className: 'warn', text: yt.problem }));

  const hasDirect = yt.videos.some((v) => v.url) && yt.audios.some((a) => a.url);
  if (hasDirect) {
    const best = pickBestPair(yt);
    const select = el('select');
    for (const video of yt.videos) {
      select.append(
        el('option', {
          value: String(video.itag),
          text: describeFormat(video),
          disabled: !video.url,
          selected: best?.video.itag === video.itag,
        }),
      );
    }
    const button = el('button', {
      className: 'primary',
      text: 'Download + merge',
      disabled: !host.ffmpeg,
      title: host.ffmpeg ? 'Downloads video and audio, then muxes them with ffmpeg' : 'ffmpeg.exe was not found next to the host',
      onclick: () => {
        const video = yt.videos.find((v) => v.itag === Number(select.value));
        const audio = video ? matchingAudio(yt, video) : null;
        if (!video || !audio) {
          toast('No compatible audio stream for that video format', true);
          return;
        }
        send<{ jobId: string }>({ type: 'download-youtube-merge', tabId, videoItag: video.itag, audioItag: audio.itag })
          .then(() => toast('Added to downloads'))
          .catch(fail);
      },
    });
    card.append(el('div', { className: 'row' }, select, button));
  } else if (!yt.problem) {
    card.append(
      el('div', { className: 'warn', text: 'YouTube protected these streams (ciphered URLs). Use yt-dlp below.' }),
    );
  }
  box.append(card);
}

// ------------------------------------------------------------------- yt-dlp ----

let ytdlpInfo: YtInfo | null = null;
let ytdlpInfoUrl = '';
let ytdlpLoading = false;
let lastYtdlpSignature = '';

function formatChoices(info: YtInfo): { label: string; selector: string }[] {
  const choices: { label: string; selector: string }[] = [{ label: 'Best available (automatic)', selector: '' }];
  const videos = info.formats.filter((f) => f.vcodec !== 'none' && f.height > 0);
  videos.sort((a, b) => b.height - a.height || b.fps - a.fps || b.tbr - a.tbr);
  for (const f of videos) {
    const hasAudio = f.acodec !== 'none';
    const label = [
      `${f.height}p${f.fps > 30 ? Math.round(f.fps) : ''}`,
      f.ext,
      f.vcodec.split('.')[0],
      f.filesize > 0 ? formatBytes(f.filesize) : '',
      hasAudio ? '' : '+ best audio',
    ]
      .filter(Boolean)
      .join(' · ');
    choices.push({ label, selector: hasAudio ? f.formatId : `${f.formatId}+bestaudio/b` });
  }
  const audios = info.formats.filter((f: YtFormat) => f.vcodec === 'none' && f.acodec !== 'none');
  audios.sort((a, b) => b.tbr - a.tbr);
  for (const f of audios.slice(0, 3)) {
    choices.push({ label: `Audio only · ${f.ext} · ${Math.round(f.tbr)} kbps`, selector: f.formatId });
  }
  return choices;
}

function startYtdlp(selector: string): void {
  send<{ jobId: string }>({
    type: 'download-ytdlp',
    url: tabUrl,
    title: ytdlpInfo?.title || tabTitle,
    pageUrl: tabUrl,
    ...(selector ? { formatSelector: selector } : {}),
  })
    .then(() => toast('Added to downloads'))
    .catch(fail);
}

function renderYtdlp(): void {
  const supported = /^https?:\/\//i.test(tabUrl);
  const infoForTab = ytdlpInfoUrl === tabUrl ? ytdlpInfo : null;
  const signature = `${tabUrl}|${host.connected}|${host.ytdlp}|${infoForTab?.id ?? ''}|${ytdlpLoading}`;
  if (signature === lastYtdlpSignature) return;
  lastYtdlpSignature = signature;
  const box = $('ytdlp');
  box.replaceChildren();
  if (!supported || !host.connected) return;

  if (!host.ytdlp) {
    box.append(
      el(
        'div',
        { className: 'card' },
        el('div', { className: 'sub', text: 'Put yt-dlp.exe next to the host (C:\\Program Files\\IDMClone) to download from video sites, HLS/DASH streams and protected YouTube videos.' }),
      ),
    );
    return;
  }

  const card = el(
    'div',
    { className: 'card' },
    el('h3', { text: 'Download this page with yt-dlp' }),
    el('div', { className: 'sub', text: `${hostOf(tabUrl)} · supports YouTube and hundreds of other sites` }),
  );

  if (infoForTab) {
    const choices = formatChoices(infoForTab);
    const select = el('select');
    choices.forEach((choice, index) => select.append(el('option', { value: String(index), text: choice.label })));
    card.append(
      el(
        'div',
        { className: 'row' },
        select,
        el('button', {
          className: 'primary',
          text: 'Download',
          onclick: () => startYtdlp(choices[Number(select.value)]?.selector ?? ''),
        }),
      ),
    );
  } else {
    card.append(
      el(
        'div',
        { className: 'row' },
        el('button', { className: 'primary', text: 'Best quality', onclick: () => startYtdlp('') }),
        el('button', {
          text: ytdlpLoading ? 'Loading formats...' : 'Choose quality...',
          disabled: ytdlpLoading,
          onclick: () => {
            ytdlpLoading = true;
            renderYtdlp();
            send<YtInfo>({ type: 'ytdlp-formats', url: tabUrl })
              .then((info) => {
                ytdlpInfo = info;
                ytdlpInfoUrl = tabUrl;
              })
              .catch(fail)
              .finally(() => {
                ytdlpLoading = false;
                renderYtdlp();
              });
          },
        }),
      ),
    );
  }
  box.append(card);
}

// ------------------------------------------------------------------ downloads ----

type Action = 'pause' | 'resume' | 'cancel' | 'remove' | 'open_folder';

function jobDisplayName(job: HostJob): string {
  return job.fileName || job.title || job.url;
}

function jobMeta(job: HostJob): string {
  if (job.state === 'FAILED') return job.error || 'Failed';
  if (job.state === 'COMPLETED') return `${formatBytes(job.total)} · done`;
  if (job.state === 'QUEUED') return 'Waiting in queue';
  if (job.state === 'PAUSED') return `Paused · ${job.percent.toFixed(1)}%`;
  const parts: string[] = [];
  if (job.state === 'MERGING') parts.push('Merging...');
  parts.push(`${job.percent.toFixed(1)}%`);
  if (job.total > 0) parts.push(`${formatBytes(job.downloaded)} / ${formatBytes(job.total)}`);
  else if (job.downloaded > 0) parts.push(formatBytes(job.downloaded));
  if (job.state === 'DOWNLOADING' && job.speed > 0) parts.push(`${formatBytes(job.speed)}/s`);
  if (job.state === 'DOWNLOADING' && job.eta >= 0) parts.push(`ETA ${formatDuration(job.eta)}`);
  return parts.join(' · ');
}

class JobRow {
  readonly root: HTMLElement;
  private readonly name = el('div', { className: 'job-name' });
  private readonly badge = el('span', { className: 'badge' });
  private readonly fill = el('div', { className: 'fill' });
  private readonly meta = el('div', { className: 'job-meta' });
  private readonly buttons = new Map<Action, HTMLButtonElement>();

  constructor(private job: HostJob) {
    const actions = el('div', { className: 'job-actions' });
    const add = (action: Action, label: string, className = ''): void => {
      const button = el('button', { text: label, className, onclick: () => this.run(action) });
      this.buttons.set(action, button);
      actions.append(button);
    };
    add('pause', 'Pause');
    add('resume', 'Resume');
    add('open_folder', 'Open folder');
    add('cancel', 'Cancel', 'danger');
    add('remove', 'Remove from list', 'danger');
    this.root = el(
      'div',
      { className: 'card' },
      el('div', { className: 'job-top' }, this.name, this.badge),
      el('div', { className: 'bar' }, this.fill),
      this.meta,
      actions,
    );
    this.update(job);
  }

  private run(action: Action): void {
    const doIt = (): void => {
      send<unknown>({ type: 'job-action', action, jobId: this.job.id }).catch(fail);
    };
    if (action === 'cancel' && !confirm('Cancel this download and delete the partial file?')) return;
    doIt();
  }

  update(job: HostJob): void {
    this.job = job;
    this.name.textContent = jobDisplayName(job);
    this.name.title = job.url;
    this.badge.textContent = job.state;
    this.badge.className = `badge state-${job.state}`;
    this.fill.style.width = `${Math.max(0, Math.min(100, job.state === 'COMPLETED' ? 100 : job.percent))}%`;
    this.fill.className = `fill${job.state === 'COMPLETED' ? ' done' : job.state === 'FAILED' ? ' failed' : ''}`;
    this.meta.textContent = jobMeta(job);
    this.meta.className = `job-meta${job.state === 'FAILED' ? ' error-text' : ''}`;
    this.meta.title = job.state === 'FAILED' ? job.error : '';

    const running = job.state === 'DOWNLOADING' || job.state === 'MERGING';
    const show = (action: Action, visible: boolean): void => {
      const button = this.buttons.get(action);
      if (button) button.hidden = !visible;
    };
    show('pause', running || job.state === 'QUEUED');
    show('resume', job.state === 'PAUSED' || job.state === 'FAILED');
    show('open_folder', job.state === 'COMPLETED');
    show('cancel', job.state !== 'COMPLETED');
    show('remove', job.state === 'COMPLETED');
    const resume = this.buttons.get('resume');
    if (resume) resume.textContent = job.state === 'FAILED' ? 'Retry' : 'Resume';
  }
}

const rows = new Map<string, JobRow>();
let renderQueued = false;

function scheduleRenderJobs(): void {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => {
    renderQueued = false;
    renderJobs();
  });
}

function renderJobs(): void {
  for (const [id, row] of rows) {
    if (!jobs.has(id)) {
      row.root.remove();
      rows.delete(id);
    }
  }
  const sorted = [...jobs.values()].sort((a, b) => b.createdAt - a.createdAt || (a.id < b.id ? 1 : -1));
  let structureChanged = false;
  for (const job of sorted) {
    let row = rows.get(job.id);
    if (!row) {
      row = new JobRow(job);
      rows.set(job.id, row);
      structureChanged = true;
    }
    row.update(job);
  }
  // Reordering moves DOM nodes (which can swallow a click), so only do it when the set changed.
  if (structureChanged) {
    $('jobList').replaceChildren(...sorted.map((job) => rows.get(job.id)!.root));
  }
  const unfinished = sorted.filter((j) => j.state !== 'COMPLETED').length;
  $('jobCount').textContent = String(unfinished);
  $('jobsEmpty').hidden = sorted.length > 0;
}

$('pauseAll').addEventListener('click', () => {
  send<null>({ type: 'pause-all' }).catch(fail);
});

// ------------------------------------------------------------------ settings ----

let settingsDirty = false;
let settingsFilled = false;

function fillSettings(state: PopupState): void {
  if (settingsDirty) return; // never overwrite what the user is typing
  if (state.settings && !settingsFilled) {
    $<HTMLInputElement>('setMax').value = String(state.settings.maxConcurrent);
    $<HTMLInputElement>('setSegments').value = String(state.settings.segments);
    $<HTMLInputElement>('setFolder').value = state.settings.defaultFolder;
    $<HTMLSelectElement>('setCookies').value = state.settings.cookiesFromBrowser;
    settingsFilled = true;
  }
  $<HTMLInputElement>('setIntercept').checked = state.interceptDownloads;
  $<HTMLInputElement>('setWindow').checked = state.openWindowOnIntercept;
}

$('settingsForm').addEventListener('input', () => {
  settingsDirty = true;
});

$('settingsForm').addEventListener('submit', (event) => {
  event.preventDefault();
  void (async () => {
    try {
      await send<null>({
        type: 'set-intercept',
        enabled: $<HTMLInputElement>('setIntercept').checked,
        openWindow: $<HTMLInputElement>('setWindow').checked,
      });
      await send<unknown>({
        type: 'set-settings',
        settings: {
          maxConcurrent: Number($<HTMLInputElement>('setMax').value),
          segments: Number($<HTMLInputElement>('setSegments').value),
          defaultFolder: $<HTMLInputElement>('setFolder').value.trim(),
          cookiesFromBrowser: $<HTMLSelectElement>('setCookies').value,
        },
      });
      settingsDirty = false;
      settingsFilled = false;
      toast('Settings saved');
      await refresh();
    } catch (err) {
      fail(err);
    }
  })();
});

// ------------------------------------------------------------- refresh + push ----

async function refresh(): Promise<void> {
  try {
    const state = await send<PopupState>({ type: 'get-state', tabId });
    currentState = state;
    renderHost(state.host);
    jobs.clear();
    state.jobs.forEach((job) => jobs.set(job.id, job));
    renderJobs();
    renderYouTube(state.youtube);
    renderYtdlp();
    renderMedia(state.media);
    fillSettings(state);
  } catch (err) {
    renderHost({ ...host, connected: false, lastError: err instanceof Error ? err.message : String(err) });
  }
}

function handlePush(push: PopupPush): void {
  switch (push.event) {
    case 'bridge':
      renderHost(push.status);
      break;
    case 'ready':
      void refresh();
      break;
    case 'progress':
      push.jobs.forEach((job) => jobs.set(job.id, job));
      scheduleRenderJobs();
      break;
    case 'state':
      jobs.set(push.job.id, push.job);
      scheduleRenderJobs();
      break;
    case 'removed':
      jobs.delete(push.id);
      scheduleRenderJobs();
      break;
  }
}

function connectPort(): void {
  // A long-lived port also keeps the service worker alive while the popup is open.
  const port = chrome.runtime.connect({ name: 'popup' });
  port.onMessage.addListener((message: PopupPush) => handlePush(message));
  port.onDisconnect.addListener(() => {
    void chrome.runtime.lastError; // mark as handled
    setTimeout(connectPort, 1000); // the service worker was restarted
  });
}

async function init(): Promise<void> {
  if (windowMode) {
    document.querySelector<HTMLButtonElement>('.tabs button[data-tab="media"]')!.hidden = true;
    activateTab('downloads');
  } else {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    tabId = tab?.id ?? -1;
    tabUrl = tab?.url ?? '';
    tabTitle = tab?.title ?? '';
  }
  connectPort();
  await refresh();
  setInterval(() => void refresh(), 2000); // picks up newly detected media and host status
}

void init();
