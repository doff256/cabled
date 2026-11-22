// Generates the Cabled app icon ("framing iris": four aperture blades closing on a square, a subject dot inside).
//   node design/icon/make-icon.mjs
// Writes:
//   ios/AppIcon.icon/            Icon Composer package (icon.json + one SVG per layer). iOS 26 adds the Liquid Glass;
//                                Xcode makes flat fallback icons for iOS 17/18 from the same file. No effects are drawn here.
//   design/icon/out/flat.svg     flat composite on a rounded tile, for Windows (rendered to Cabled.ico by make-ico.ps1)
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const S = 1024, k = S / 100;              // design grid is 100×100, scaled to Apple's 1024 canvas
const CREAM = '#f7ebe8';
const TOP = [0xa8, 0x49, 0x3e], BOTTOM = [0x5a, 0x24, 0x1f];   // mangrove lacquer, top to bottom

// Pinwheel aperture: N blades between an inner polygon (the opening, vertex radius r) and an outer circle R.
function iris(N, r, R, rot, cx = 50, cy = 50) {
  const v = [], X = [];
  for (let i = 0; i < N; i++) { const a = rot + i * 2 * Math.PI / N; v.push([r * Math.cos(a), r * Math.sin(a)]); }
  for (let i = 0; i < N; i++) {
    const q = v[i], p = v[(i + 1) % N];
    let dx = p[0] - q[0], dy = p[1] - q[1]; const L = Math.hypot(dx, dy); dx /= L; dy /= L;
    const pd = p[0] * dx + p[1] * dy, t = -pd + Math.sqrt(pd * pd - (p[0] ** 2 + p[1] ** 2) + R * R);
    X.push([p[0] + t * dx, p[1] + t * dy]);
  }
  const pt = a => `${((cx + a[0]) * k).toFixed(2)} ${((cy + a[1]) * k).toFixed(2)}`;
  return v.map((_, i) => `M${pt(v[i])} L${pt(v[(i + 1) % N])} L${pt(X[i])} A${R * k} ${R * k} 0 0 0 ${pt(X[(i - 1 + N) % N])} Z`);
}
const blades = iris(4, 19, 36, Math.PI / 4);
const bladeA = blades.filter((_, i) => i % 2 === 0).map(d => `<path d="${d}" fill="${CREAM}"/>`).join('');
const bladeB = blades.filter((_, i) => i % 2 === 1).map(d => `<path d="${d}" fill="${CREAM}"/>`).join('');
const subject = `<circle cx="${50 * k}" cy="${50 * k}" r="${5 * k}" fill="${CREAM}"/>`;
const svg = (body, size = S) => `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${S} ${S}">${body}</svg>\n`;

// Icon Composer package. Layers are opaque and unmasked (Apple: set opacity and effects in the .icon, not the art).
const pkg = join(repo, 'ios', 'AppIcon.icon');
rmSync(pkg, { recursive: true, force: true });
mkdirSync(join(pkg, 'Assets'), { recursive: true });
const layers = [   // top first, as Icon Composer lists them
  { name: '3 Blades A', body: bladeA, opacity: 1 },
  { name: '2 Blades B', body: bladeB, opacity: 0.6 },
  { name: '1 Subject', body: subject, opacity: 1 },
];
for (const l of layers) writeFileSync(join(pkg, 'Assets', `${l.name}.svg`), svg(l.body));
const srgb = c => `srgb:${c.map(x => (x / 255).toFixed(5)).join(',')},1.00000`;
const icon = {
  fill: { 'linear-gradient': [srgb(TOP), srgb(BOTTOM)], orientation: { start: { x: 0.5, y: 0 }, stop: { x: 0.5, y: 1 } } },
  groups: layers.map(l => ({
    layers: [{ glass: true, 'image-name': `${l.name}.svg`, name: l.name, ...(l.opacity < 1 ? { opacity: l.opacity } : {}) }],
    shadow: { kind: 'neutral', opacity: 0.5 },
    translucency: { enabled: true, value: 0.4 },
  })),
  'supported-platforms': { squares: 'shared' },
};
writeFileSync(join(pkg, 'icon.json'), JSON.stringify(icon, null, 2) + '\n');

// Flat composite for Windows: same art on a rounded mangrove tile with a small inset.
const hex = c => '#' + c.map(x => x.toString(16).padStart(2, '0')).join('');
const inset = 0.03 * S, side = S - 2 * inset, radius = 0.2 * side;
const flat = `<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${hex(TOP)}"/><stop offset="1" stop-color="${hex(BOTTOM)}"/></linearGradient>
<clipPath id="tile"><rect x="${inset}" y="${inset}" width="${side}" height="${side}" rx="${radius}"/></clipPath></defs>
<g clip-path="url(#tile)"><rect width="${S}" height="${S}" fill="url(#bg)"/>${subject}<g opacity="0.6">${bladeB}</g>${bladeA}</g>`;
mkdirSync(join(here, 'out'), { recursive: true });
writeFileSync(join(here, 'out', 'flat.svg'), svg(flat));
console.log('wrote', pkg, 'and design/icon/out/flat.svg');
