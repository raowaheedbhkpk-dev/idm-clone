// nativeBridge.ts - typed request/response + event wrapper around chrome.runtime.connectNative.
//
// Note: the API for talking to a native host is chrome.runtime.connectNative().
// (chrome.runtime.onConnectNative exists, but it is for the opposite direction -
// a native app connecting to the extension - and is ChromeOS-only.)

import type { BridgeStatus, Command, CommandMap, HostEvent } from './types.js';

export const HOST_NAME = 'com.idmclone.host';

interface Pending {
  resolve: (value: never) => void;
  reject: (reason: Error) => void;
  timer: ReturnType<typeof setTimeout>;
  cmd: string;
}

interface HostResponse {
  id: number;
  ok: boolean;
  error?: string;
  [key: string]: unknown;
}

const MIN_BACKOFF_MS = 1000;
const MAX_BACKOFF_MS = 30000;

export class NativeBridge {
  private port: chrome.runtime.Port | null = null;
  private nextId = 1;
  private readonly pending = new Map<number, Pending>();
  private backoffMs = MIN_BACKOFF_MS;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private readonly eventListeners = new Set<(event: HostEvent) => void>();
  private readonly statusListeners = new Set<(status: BridgeStatus) => void>();

  status: BridgeStatus = { connected: false, lastError: '', hostVersion: '', ffmpeg: false, ytdlp: false };

  constructor(private readonly hostName: string = HOST_NAME) {}

  isConnected(): boolean {
    return this.port !== null;
  }

  onEvent(listener: (event: HostEvent) => void): void {
    this.eventListeners.add(listener);
  }

  onStatus(listener: (status: BridgeStatus) => void): void {
    this.statusListeners.add(listener);
  }

  /** Opens the port (idempotent). Failures are retried with exponential back-off. */
  connect(): void {
    if (this.port) return;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    let port: chrome.runtime.Port;
    try {
      port = chrome.runtime.connectNative(this.hostName);
    } catch (err) {
      this.handleDisconnect(err instanceof Error ? err.message : String(err));
      return;
    }
    this.port = port;
    port.onMessage.addListener((message: unknown) => this.handleMessage(message));
    port.onDisconnect.addListener(() => {
      // Reading lastError is mandatory, otherwise Chrome logs "Unchecked runtime.lastError".
      const reason = chrome.runtime.lastError?.message ?? 'Native host disconnected';
      this.handleDisconnect(reason);
    });
    // Connected is confirmed by the host's "ready" event; a missing host fails right after connect.
  }

  /** Drops the current port and reconnects immediately. */
  reconnectNow(): void {
    this.backoffMs = MIN_BACKOFF_MS;
    if (this.port) {
      this.port.disconnect();
      this.port = null;
    }
    this.failAllPending(new Error('Reconnecting to native host'));
    this.connect();
  }

  request<C extends Command>(
    cmd: C,
    payload: CommandMap[C]['req'],
    timeoutMs = 30000,
  ): Promise<CommandMap[C]['res']> {
    const port = this.port;
    if (!port) {
      const detail = this.status.lastError ? ` (${this.status.lastError})` : '';
      return Promise.reject(new Error(`Native host is not connected${detail}`));
    }
    const id = this.nextId++;
    return new Promise<CommandMap[C]['res']>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Native host did not answer '${cmd}' in ${Math.round(timeoutMs / 1000)} s`));
      }, timeoutMs);
      this.pending.set(id, { resolve: resolve as (v: never) => void, reject, timer, cmd });
      try {
        port.postMessage({ ...payload, id, cmd });
      } catch (err) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(err instanceof Error ? err : new Error(String(err)));
      }
    });
  }

  // ------------------------------------------------------------ internals --

  private handleMessage(message: unknown): void {
    if (typeof message !== 'object' || message === null) return;
    const record = message as Record<string, unknown>;

    if (typeof record.event === 'string') {
      const event = message as HostEvent;
      if (event.event === 'ready') {
        this.backoffMs = MIN_BACKOFF_MS; // a healthy host: forget previous failures
        this.status = { ...this.status, connected: true, lastError: '', hostVersion: event.version };
        this.notifyStatus();
        void this.probeCapabilities();
      }
      this.eventListeners.forEach((listener) => listener(event));
      return;
    }

    const response = message as HostResponse;
    const entry = this.pending.get(response.id);
    if (!entry) return;
    clearTimeout(entry.timer);
    this.pending.delete(response.id);
    if (response.ok) {
      entry.resolve(response as never);
    } else {
      entry.reject(new Error(response.error ?? `Host command '${entry.cmd}' failed`));
    }
  }

  private async probeCapabilities(): Promise<void> {
    try {
      const info = await this.request('ping', {});
      this.status = { ...this.status, ffmpeg: info.ffmpeg, ytdlp: info.ytdlp, hostVersion: info.version };
      this.notifyStatus();
    } catch {
      /* the next reconnect will probe again */
    }
  }

  private handleDisconnect(reason: string): void {
    this.port = null;
    this.status = { ...this.status, connected: false, lastError: reason };
    this.failAllPending(new Error(reason));
    this.notifyStatus();
    if (this.reconnectTimer) return;
    const delay = this.backoffMs;
    this.backoffMs = Math.min(this.backoffMs * 2, MAX_BACKOFF_MS);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delay);
  }

  private failAllPending(error: Error): void {
    for (const [id, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(error);
      this.pending.delete(id);
    }
  }

  private notifyStatus(): void {
    this.statusListeners.forEach((listener) => listener(this.status));
  }
}
