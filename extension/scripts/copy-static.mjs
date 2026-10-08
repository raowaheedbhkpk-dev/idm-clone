// Copies the non-TypeScript files next to the compiled JavaScript so that `dist/` is a
// complete, loadable unpacked extension, then sanity-checks the manifest.
import { cpSync, existsSync, mkdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');
mkdirSync(dist, { recursive: true });

cpSync(join(root, 'manifest.json'), join(dist, 'manifest.json'));
cpSync(join(root, 'src', 'popup.html'), join(dist, 'popup.html'));
cpSync(join(root, 'src', 'popup.css'), join(dist, 'popup.css'));
cpSync(join(root, 'icons'), join(dist, 'icons'), { recursive: true });

// Every file the manifest points at must exist, otherwise Chrome refuses to load it.
const manifest = JSON.parse(readFileSync(join(dist, 'manifest.json'), 'utf8'));
const referenced = [
  manifest.background?.service_worker,
  manifest.action?.default_popup,
  ...Object.values(manifest.icons ?? {}),
  ...(manifest.content_scripts ?? []).flatMap((entry) => entry.js ?? []),
].filter(Boolean);
const missing = referenced.filter((file) => !existsSync(join(dist, file)));
if (missing.length > 0) {
  console.error(`manifest.json references missing files: ${missing.join(', ')}`);
  process.exit(1);
}
console.log(`Extension built in ${dist} (${referenced.length} manifest entries verified)`);
