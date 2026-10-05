import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

// Documentation only: this does not build, install or restart Mullion.
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const original = readFileSync(resolve(root, 'docs/index.html'), 'latin1');
if (!original.includes('<head>') || /<base\b/i.test(original)) throw new Error('Unexpected homepage structure');
const newline = original.includes('\r\n') ? '\r\n' : '\n';
// Relative assets, feature navigation and dynamically generated links resolve
// to the same docs directory on both the custom domain and repository prefix.
const home = original.replace('<head>', '<head>' + newline + '<base href="../">')
	.replaceAll('href="#main"', 'href="home/#main"');
mkdirSync(resolve(root, 'docs/home'), { recursive: true });
writeFileSync(resolve(root, 'docs/home/index.html'), home, 'latin1');
console.log('Generated full original feature-tour /home; terminal binaries untouched');
