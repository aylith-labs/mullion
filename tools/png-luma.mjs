// Mean luminance of a PNG, 0 (black) to 1 (white), with no dependencies.
// check-site.mjs uses it to catch a capture whose pixels belong to the other
// theme: a *-dark.png that is mostly light, or a *-light.png that is mostly dark
// (Canary's theme not having switched before the shot was taken).
//
// Handles what the site ships: 8-bit RGB, RGBA, greyscale and greyscale+alpha,
// not interlaced. Anything else returns null and is left unjudged.
import { readFileSync } from "node:fs";
import { inflateSync } from "node:zlib";

const CHANNELS = { 0: 1, 2: 3, 4: 2, 6: 4 };

export function meanLuma(file) {
  const buf = readFileSync(file);
  let width = 0, height = 0, depth = 0, type = 0, interlace = 0;
  const idat = [];
  for (let at = 8; at < buf.length; ) {
    const len = buf.readUInt32BE(at);
    const kind = buf.toString("latin1", at + 4, at + 8);
    const data = buf.subarray(at + 8, at + 8 + len);
    if (kind === "IHDR") {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      depth = data[8]; type = data[9]; interlace = data[12];
    } else if (kind === "IDAT") idat.push(data);
    else if (kind === "IEND") break;
    at += 12 + len;
  }
  const ch = CHANNELS[type];
  if (depth !== 8 || !ch || interlace) return null;

  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * ch;
  let prev = Buffer.alloc(stride);
  let sum = 0, weight = 0;
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)));
    for (let i = 0; i < stride; i++) {
      const a = i >= ch ? line[i - ch] : 0, b = prev[i], c = i >= ch ? prev[i - ch] : 0;
      let add = 0;
      if (filter === 1) add = a;
      else if (filter === 2) add = b;
      else if (filter === 3) add = (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      line[i] = (line[i] + add) & 255;
    }
    // Every fourth pixel is plenty for a mean, and keeps the check fast.
    for (let x = 0; x < width; x += 4) {
      const p = x * ch;
      const alpha = ch === 4 ? line[p + 3] / 255 : ch === 2 ? line[p + 1] / 255 : 1;
      if (!alpha) continue;
      const l = ch >= 3 ? 0.2126 * line[p] + 0.7152 * line[p + 1] + 0.0722 * line[p + 2] : line[p];
      sum += (l / 255) * alpha;
      weight += alpha;
    }
    prev = line;
  }
  return weight ? sum / weight : null;
}
