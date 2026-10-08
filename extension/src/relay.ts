// relay.ts - isolated-world half of the content script.
//
// content.ts runs in the page's world (no chrome.* APIs). It posts what it finds to
// window; this script - which does have chrome.runtime - forwards it to the
// background service worker. Classic script: no import/export.

(() => {
  const SOURCE = 'idmclone-page';

  window.addEventListener('message', (event: MessageEvent<unknown>) => {
    // Only accept messages that this very frame's page-world script posted to itself.
    if (event.source !== window) return;
    const data = event.data as { source?: unknown; type?: unknown } | null;
    if (typeof data !== 'object' || data === null || data.source !== SOURCE) return;
    if (data.type !== 'media-detected' && data.type !== 'youtube-player') return;

    const { source: _source, ...message } = data as Record<string, unknown>;
    void _source;
    try {
      chrome.runtime.sendMessage(message).catch(() => undefined); // service worker may be starting
    } catch {
      /* "Extension context invalidated" after the extension was reloaded */
    }
  });
})();
