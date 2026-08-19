// Generates simulator fixtures using the real gateway code.
//
// The raw inputs here stand in for what PDW's dashboard query returns, but
// everything downstream — label formatting, relative times, truncation, and the
// Floyd-Steinberg reduction to the panel palette — runs through the actual
// functions in server/dashboard.mjs. A fixture is therefore a genuine gateway
// payload, not a hand-written approximation of one, and no PDW access or
// hardware is needed to produce it.

import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { normalizeDashboard, quantizeBmp } from '../server/dashboard.mjs';

const SIM_DIR = dirname(fileURLToPath(import.meta.url));
const FIXTURE_DIR = join(SIM_DIR, 'fixtures');
const PHOTO_WIDTH = 270;
const PHOTO_HEIGHT = 250;

// A fixed clock keeps relative labels ("IN 24 MIN") and therefore rendered
// output byte-identical between runs, so PNG diffs mean something.
const NOW = new Date('2026-08-18T22:00:00-04:00');

function buildBmp(width, height, shade) {
  const rowBytes = Math.ceil(width * 3 / 4) * 4;
  const pixelOffset = 54;
  const size = pixelOffset + rowBytes * height;
  const bitmap = Buffer.alloc(size);
  bitmap.write('BM', 0, 'ascii');
  bitmap.writeUInt32LE(size, 2);
  bitmap.writeUInt32LE(pixelOffset, 10);
  bitmap.writeUInt32LE(40, 14);
  bitmap.writeInt32LE(width, 18);
  bitmap.writeInt32LE(height, 22);
  bitmap.writeUInt16LE(1, 26);
  bitmap.writeUInt16LE(24, 28);
  bitmap.writeUInt32LE(0, 30);
  for (let y = 0; y < height; y += 1) {
    // BMP rows with positive height are bottom-up.
    const row = pixelOffset + (height - 1 - y) * rowBytes;
    for (let x = 0; x < width; x += 1) {
      const [red, green, blue] = shade(x / (width - 1), y / (height - 1));
      const offset = row + x * 3;
      bitmap[offset] = Math.max(0, Math.min(255, Math.round(blue)));
      bitmap[offset + 1] = Math.max(0, Math.min(255, Math.round(green)));
      bitmap[offset + 2] = Math.max(0, Math.min(255, Math.round(red)));
    }
  }
  return bitmap;
}

// A synthetic outdoor scene: smooth sky gradient, a warm sun, and a darker
// foreground. Smooth gradients are the hard case for a six-colour panel, so
// this shows the dither honestly.
function scenePhoto() {
  return buildBmp(PHOTO_WIDTH, PHOTO_HEIGHT, (u, v) => {
    if (v < 0.62) {
      const sky = v / 0.62;
      let red = 120 + sky * 90;
      let green = 165 + sky * 70;
      let blue = 225 - sky * 35;
      const sunX = u - 0.68;
      const sunY = v - 0.22;
      const sun = Math.hypot(sunX * 1.1, sunY);
      if (sun < 0.16) {
        const core = 1 - sun / 0.16;
        red += core * 130;
        green += core * 95;
        blue += core * 20;
      }
      return [red, green, blue];
    }
    const ground = (v - 0.62) / 0.38;
    const ridge = Math.sin(u * 7.5) * 0.045;
    if (v < 0.68 + ridge) return [96 - ground * 20, 118 - ground * 25, 92 - ground * 20];
    return [
      74 - ground * 30 + Math.sin(u * 21) * 7,
      96 - ground * 34 + Math.cos(u * 17) * 8,
      63 - ground * 26,
    ];
  });
}

function photoPayload(capturedAt) {
  return {
    capturedAt,
    width: PHOTO_WIDTH,
    height: PHOTO_HEIGHT,
    encoding: 'gxepd7c-4bpp',
    pixels: quantizeBmp(scenePhoto()).toString('base64'),
  };
}

// A realistic candidate pool: far more entries than fit, clustered into bursts,
// so the gateway's bucketing has something to actually spread.
//
// Every person and message below is invented, and must stay that way: this
// repository is public, so fixtures must never be seeded from real payloads.
// Generating one from a live gateway response would publish real contacts and
// real private messages. See AGENTS.md.
const TIMELINE = [
  { occurredAt: '2026-08-18T07:58:00-04:00', source: 'apple_messages', actor: 'Robin', context: 'Robin', summary: 'Confirmed the Thursday walkthrough' },
  { occurredAt: '2026-08-18T08:12:00-04:00', source: 'apple_messages', actor: 'me', context: 'Robin', summary: 'See you then' },
  { occurredAt: '2026-08-18T09:40:00-04:00', source: 'gmail', actor: 'me', context: 'grants', summary: 'Sent the grant renewal packet' },
  { occurredAt: '2026-08-18T10:15:00-04:00', source: 'slack', actor: 'Priya', context: '#hardware', summary: 'Pushed the panel driver patch for review' },
  { occurredAt: '2026-08-18T11:05:00-04:00', source: 'slack', actor: 'Priya', context: '#hardware', summary: 'Shipped the onboarding fix to prod' },
  { occurredAt: '2026-08-18T12:31:00-04:00', source: 'whatsapp', actor: 'Dad', context: 'Dad', summary: 'Photos from the lake house' },
  { occurredAt: '2026-08-18T13:58:00-04:00', source: 'slack', actor: 'me', context: '#leaders', summary: 'Reviewed the club leader applications' },
  { occurredAt: '2026-08-18T14:47:00-04:00', source: 'apple_notes', actor: 'me', context: 'notes', summary: 'Drafted the fall program outline' },
  { occurredAt: '2026-08-18T16:20:00-04:00', source: 'gmail', actor: 'Marcus Webb', context: 'vendors', summary: 'Quote for the enclosure run' },
  { occurredAt: '2026-08-18T18:04:00-04:00', source: 'slack', actor: 'jules', context: '#design', summary: 'New masthead options are in Figma' },
  { occurredAt: '2026-08-18T20:11:00-04:00', source: 'whatsapp', actor: 'Dana Reyes', context: 'Dana', summary: 'Sounds good, talk tomorrow' },
  { occurredAt: '2026-08-18T21:29:00-04:00', source: 'whatsapp', actor: 'Marcus Webb', context: 'Marcus', summary: 'Still waiting on the supplier to confirm' },
  { occurredAt: '2026-08-18T21:29:30-04:00', source: 'whatsapp', actor: 'Marcus Webb', context: 'Marcus', summary: 'Sending the spec sheet over now' },
  { occurredAt: '2026-08-18T21:42:00-04:00', source: 'slack', actor: 'sam', context: '#ops', summary: 'can you rotate the deploy key this week' },
];

// Raw Slack markup and membership events, exactly as they arrive. Nothing here
// should survive to the panel in its original form. Identifiers are invented.
const NOISY_TIMELINE = [
  { occurredAt: '2026-08-18T09:02:00-04:00', source: 'slack', actor: 'me', context: '#ops', summary: '<@U01EXAMPLE1> has joined the channel', mentions: { U01EXAMPLE1: 'sam' } },
  { occurredAt: '2026-08-18T10:30:00-04:00', source: 'slack', actor: 'sam', context: '#ops', summary: '<@U02EXAMPLE2> can you rotate the deploy key this week', mentions: { U02EXAMPLE2: 'jules' } },
  { occurredAt: '2026-08-18T11:45:00-04:00', source: 'slack', actor: 'me', context: '#design', summary: 'set the channel topic: shipping the E1002 build' },
  { occurredAt: '2026-08-18T13:10:00-04:00', source: 'slack', actor: 'Priya', context: '#hardware', summary: 'see <https://good-display.com/product/533.html|the datasheet> and ping <!here>' },
  { occurredAt: '2026-08-18T15:22:00-04:00', source: 'slack', actor: 'jules', context: '#design', summary: 'moved it to <#C09ABCDEF|design-review> — thoughts &amp; feedback welcome' },
  { occurredAt: '2026-08-18T17:40:00-04:00', source: 'slack', actor: 'me', context: '#lounge', summary: 'shared a file' },
  { occurredAt: '2026-08-18T19:05:00-04:00', source: 'slack', actor: 'Marcus Webb', context: '#vendors', summary: 'quote is at <https://vendor.example.com/q/8891>' },
  { occurredAt: '2026-08-18T21:15:00-04:00', source: 'slack', actor: 'sam', context: '#ops', summary: 'thanks <@U03UNKNOWN9>! *shipping* it now' },
];

// normalizeDashboard deliberately returns photo.pixels empty — it only carries
// metadata, and the loader merges the converted image in afterwards. Do the
// same here so the fixture matches what the device receives over the wire.
function build(raw, now = NOW) {
  const dashboard = normalizeDashboard(raw, now);
  const source = raw.photos?.[0];
  if (dashboard.photo && source?.pixels) {
    dashboard.photo = {
      ...dashboard.photo,
      width: source.width,
      height: source.height,
      encoding: source.encoding,
      pixels: source.pixels,
    };
  }
  return dashboard;
}

const fixtures = {};

// 1. The ordinary case: a full timeline, a photo, an upcoming meeting.
fixtures['typical-day'] = build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T21:58:00-04:00',
  calendar: [{
    title: 'Design review with the hardware team',
    location: 'Zoom',
    startAt: '2026-08-18T23:00:00-04:00',
    endAt: '2026-08-18T23:45:00-04:00',
    allDay: false,
  }],
  timeline: TIMELINE,
  photos: [photoPayload('2026-08-17T18:22:00-04:00')],
});

// 2. Early morning: every empty-state path at once.
fixtures['empty-day'] = build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T06:04:00-04:00',
  calendar: [],
  timeline: [],
  photos: [],
}, new Date('2026-08-18T06:05:00-04:00'));

// 3. Urgent transition: drives the red accent bar and the "IN n MIN" label.
fixtures['urgent'] = build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T21:58:00-04:00',
  calendar: [{
    title: 'Standup',
    location: 'Kitchen table',
    startAt: '2026-08-18T22:14:00-04:00',
    endAt: '2026-08-18T22:45:00-04:00',
    allDay: false,
  }],
  timeline: TIMELINE.slice(0, 8),
  photos: [photoPayload('2026-08-18T07:15:00-04:00')],
});

// 4. Overflow: long strings on every clipped field, to see where the ".."
//    truncation lands on the real glyph metrics.
fixtures['overflow'] = build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T21:58:00-04:00',
  calendar: [{
    title: 'Quarterly planning and roadmap alignment session with everyone',
    location: 'The very long conference room name on the third floor',
    startAt: '2026-08-19T09:00:00-04:00',
    endAt: '2026-08-19T10:30:00-04:00',
    allDay: false,
  }],
  timeline: TIMELINE.map((entry, index) => ({
    ...entry,
    actor: index % 2 ? 'Bartholomew Fitzgerald-Harrington' : entry.actor,
    summary: `${entry.summary} and then a great deal more detail than will ever fit on one line of the panel`,
  })),
  photos: [photoPayload('2026-08-16T20:02:00-04:00')],
});

// 5. Cached: PDW was unreachable and the gateway served last-good.
fixtures['cached'] = { ...build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T18:02:00-04:00',
  calendar: [{
    title: 'Library board meeting',
    location: '',
    startAt: '2026-08-19T19:00:00-04:00',
    endAt: '2026-08-19T20:00:00-04:00',
    allDay: false,
  }],
  timeline: TIMELINE.slice(0, 10),
  photos: [photoPayload('2026-08-15T09:40:00-04:00')],
}), stale: true };

// 6. Photo dropped: the gateway could not convert the image but the rest is
//    fine, so the panel shows the placeholder box.
fixtures['no-photo'] = build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T21:58:00-04:00',
  calendar: [{
    title: 'Dentist',
    location: '',
    startAt: '2026-08-19T09:00:00-04:00',
    endAt: '2026-08-19T09:45:00-04:00',
    allDay: false,
  }],
  timeline: TIMELINE.slice(0, 12),
  photos: [],
});

// 7. Slack noise: raw mention markup, links, entities, and membership events.
//    Every row here is either cleaned up or dropped before it reaches the panel.
fixtures['slack-noise'] = build({
  protocolVersion: 5,
  generatedAt: '2026-08-18T21:58:00-04:00',
  calendar: [{
    title: 'Dentist',
    location: '',
    startAt: '2026-08-19T09:00:00-04:00',
    endAt: '2026-08-19T09:45:00-04:00',
    allDay: false,
  }],
  timeline: NOISY_TIMELINE,
  photos: [photoPayload('2026-08-18T12:00:00-04:00')],
});

await mkdir(FIXTURE_DIR, { recursive: true });
for (const [name, payload] of Object.entries(fixtures)) {
  const path = join(FIXTURE_DIR, `${name}.json`);
  await writeFile(path, `${JSON.stringify(payload, null, 2)}\n`);
  const photo = payload.photo?.pixels ? 'photo' : 'no photo';
  const span = payload.timeline.length
    ? `${payload.timeline[0].timeLabel}-${payload.timeline[payload.timeline.length - 1].timeLabel}`
    : 'empty';
  console.log(`fixture ${name.padEnd(12)} ${String(payload.timeline.length).padStart(2)} rows ${span.padEnd(16)} ${photo}`);
}
console.log(`\nwrote ${Object.keys(fixtures).length} fixtures to ${resolve(FIXTURE_DIR)}`);
