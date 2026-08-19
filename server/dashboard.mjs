import { createHash, timingSafeEqual } from 'node:crypto';
import { readFile, rename, writeFile, mkdir } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DEFAULT_QUERY_PATH = resolve(ROOT, 'server/dashboard-query.sql');
const DEFAULT_TOKEN_PATH = resolve(ROOT, '.secrets/dashboard-token');
const DEFAULT_CACHE_PATH = resolve(ROOT, '.state/dashboard-cache.json');
const DEFAULT_PDW_CONFIG = resolve(process.env.HOME ?? '', '.config/pdw-cli/config.json');
const CACHE_TTL_MS = 5 * 60 * 1000;
const STALE_MAX_AGE_MS = 24 * 60 * 60 * 1000;
const TIMEZONE = 'America/New_York';
const TIMELINE_LIMIT = 6;
const PROTOCOL_VERSION = 5;
const PHOTO_WIDTH = 270;
const PHOTO_HEIGHT = 250;
const PHOTO_BYTES = PHOTO_WIDTH * PHOTO_HEIGHT / 2;
const MAX_SOURCE_PHOTO_BYTES = 5 * 1024 * 1024;

function safeEqual(left, right) {
  const a = createHash('sha256').update(left).digest();
  const b = createHash('sha256').update(right).digest();
  return timingSafeEqual(a, b);
}

function cleanText(value, maxLength = 42) {
  if (typeof value !== 'string') return '';
  const clean = value.replace(/\s+/g, ' ').trim();
  if (clean.length <= maxLength) return clean;
  return `${clean.slice(0, Math.max(0, maxLength - 1)).trimEnd()}…`;
}

function cleanDisplayText(value, maxLength) {
  if (typeof value !== 'string') return '';
  return cleanText(value
    .replace(/[‘’]/g, "'")
    .replace(/[“”]/g, '"')
    .replace(/[^\x20-\x7e]/g, ' '), maxLength);
}

function displayDate(date, options) {
  if (!date || Number.isNaN(date.valueOf())) return '—';
  return new Intl.DateTimeFormat('en-US', {
    timeZone: TIMEZONE,
    ...options,
  }).format(date);
}

// Milliseconds to add to a wall-clock-as-UTC value to get the real instant.
function zoneOffsetMs(instant, timeZone) {
  const parts = Object.fromEntries(
    new Intl.DateTimeFormat('en-US', {
      timeZone,
      hour12: false,
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
    })
      .formatToParts(instant)
      .filter((part) => part.type !== 'literal')
      .map((part) => [part.type, part.value]),
  );
  const asUtc = Date.UTC(
    Number(parts.year),
    Number(parts.month) - 1,
    Number(parts.day),
    Number(parts.hour) % 24,
    Number(parts.minute),
    Number(parts.second),
  );
  return instant.valueOf() - asUtc;
}

export function startOfLocalDay(instant, timeZone = TIMEZONE) {
  const ymd = new Intl.DateTimeFormat('en-CA', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
  }).format(instant);
  const midnightAsUtc = new Date(`${ymd}T00:00:00Z`);
  return new Date(midnightAsUtc.valueOf() + zoneOffsetMs(midnightAsUtc, timeZone));
}

function linkHost(url) {
  try {
    return new URL(url).hostname.replace(/^www\./, '');
  } catch {
    return 'link';
  }
}

// Slack stores mentions and links as markup. Unresolved, a single <@U01EXAMPLE1>
// can occupy a third of a row on a six-row display, so anything that survives to
// the panel must already be human-readable.
export function cleanSlackMarkup(value, mentions = null) {
  if (typeof value !== 'string') return '';
  return value
    .replace(/<!subteam\^[A-Z0-9]+(?:\|([^>]+))?>/g, (match, label) => label || '@team')
    .replace(/<!(here|channel|everyone)(?:\|[^>]*)?>/g, '@$1')
    .replace(/<@[UWB][A-Z0-9]+\|([^>]+)>/g, '@$1')
    .replace(/<@([UWB][A-Z0-9]+)>/g, (match, id) => {
      const name = mentions?.[id];
      return name ? `@${name}` : '@someone';
    })
    .replace(/<#C[A-Z0-9]+\|([^>]+)>/g, '#$1')
    .replace(/<#C[A-Z0-9]+>/g, '#channel')
    .replace(/<(?:https?:\/\/|mailto:)[^>|]+\|([^>]+)>/g, '$1')
    .replace(/<(https?:\/\/[^>|]+)>/g, (match, url) => linkHost(url))
    .replace(/<mailto:([^>|]+)>/g, '$1')
    .replace(/[`*]/g, '')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>');
}

// Membership and channel-administration events pass the self/direct filter
// because Zach triggered them, but they say nothing about his day.
const NOISE_PATTERNS = [
  /\bhas joined the (channel|group|conversation)\b/i,
  /\bhas left the (channel|group|conversation)\b/i,
  /\bwas (added to|removed from) the (channel|group)\b/i,
  /\badded an integration to this channel\b/i,
  /\bset the channel (topic|purpose|description)\b/i,
  /\b(renamed|archived|un-?archived) the channel\b/i,
  /\bpinned a message to this channel\b/i,
  /\bshared a file\b/i,
  /^(joined|left)\b/i,
  /this content can'?t be displayed/i,
];

export function isTimelineNoise(text) {
  if (typeof text !== 'string') return true;
  const trimmed = text.trim();
  if (trimmed.length < 3) return true;
  return NOISE_PATTERNS.some((pattern) => pattern.test(trimmed));
}

// Each source names Zach differently — "me" from Apple Messages, the Slack
// handle, the sending address from Gmail. On a six-row panel they should all
// read as one person.
const SELF_IDENTITIES = new Set([
  'me',
  'you',
  'zrl',
  'zach',
  'zach latta',
  'zach@hackclub.com',
  'zachlatta',
]);

export function displayActor(actor) {
  const clean = (actor ?? '').trim();
  if (!clean) return 'Unknown';
  return SELF_IDENTITIES.has(clean.toLowerCase()) ? 'You' : clean;
}

function conversationKey(entry) {
  return `${entry.source ?? ''}|${entry.context || entry.actor || ''}`;
}

// "The day, so far" has to mean the day. Taking the six most recent entries
// collapsed the panel into whatever burst happened last — at 11pm that was a
// thirteen-minute window. Instead, divide the elapsed part of the day into as
// many buckets as there are rows and take one representative from each, then
// backfill from the most recent leftovers.
export function selectTimelineEntries(entries, now, limit = TIMELINE_LIMIT) {
  const usable = (Array.isArray(entries) ? entries : [])
    .filter((entry) => entry && entry.occurredAt)
    .map((entry) => ({ ...entry, at: new Date(entry.occurredAt) }))
    .filter((entry) => !Number.isNaN(entry.at.valueOf()))
    .filter((entry) => !isTimelineNoise(cleanSlackMarkup(entry.summary, entry.mentions)))
    .sort((a, b) => b.at - a.at);

  const chronological = (list) => list.slice().sort((a, b) => a.at - b.at);
  if (usable.length <= limit) return chronological(usable);

  const dayStart = startOfLocalDay(now);
  const span = Math.max(1, now.valueOf() - dayStart.valueOf());
  const bucketOf = (entry) => Math.min(
    limit - 1,
    Math.max(0, Math.floor(((entry.at.valueOf() - dayStart.valueOf()) / span) * limit)),
  );

  const perConversation = new Map();
  const taken = new Set();
  const chosen = [];
  const conversationCount = (entry) => perConversation.get(conversationKey(entry)) ?? 0;
  const take = (index) => {
    const entry = usable[index];
    chosen.push(entry);
    taken.add(index);
    perConversation.set(conversationKey(entry), conversationCount(entry) + 1);
  };

  const byBucket = new Map();
  usable.forEach((entry, index) => {
    const bucket = bucketOf(entry);
    if (!byBucket.has(bucket)) byBucket.set(bucket, []);
    byBucket.get(bucket).push(index);
  });

  for (let bucket = 0; bucket < limit && chosen.length < limit; bucket += 1) {
    const candidates = byBucket.get(bucket) ?? [];
    const pick = candidates.find((index) => conversationCount(usable[index]) < 2);
    if (pick !== undefined) take(pick);
  }
  // Backfill by temporal dispersion rather than recency. Taking "the next most
  // recent" tended to pick two messages thirty seconds apart while leaving the
  // whole morning unrepresented; this picks whichever candidate sits furthest
  // from everything already on the panel.
  const backfill = (conversationCap) => {
    while (chosen.length < limit) {
      let best = -1;
      let bestDistance = -1;
      for (let index = 0; index < usable.length; index += 1) {
        if (taken.has(index)) continue;
        if (conversationCount(usable[index]) >= conversationCap) continue;
        const distance = chosen.length
          ? Math.min(...chosen.map((entry) => Math.abs(entry.at - usable[index].at)))
          : Number.POSITIVE_INFINITY;
        if (distance > bestDistance) {
          bestDistance = distance;
          best = index;
        }
      }
      if (best < 0) return;
      take(best);
    }
  };
  backfill(1);
  backfill(2);
  return chronological(chosen);
}

export function normalizeDashboard(raw, now = new Date()) {
  const parsed = typeof raw === 'string' ? JSON.parse(raw) : raw;
  if (!parsed || parsed.protocolVersion !== PROTOCOL_VERSION) {
    throw new Error('Unsupported PDW dashboard payload');
  }

  const calendar = Array.isArray(parsed.calendar)
    ? parsed.calendar.slice(0, 1).map((event) => {
        const start = event.startAt ? new Date(event.startAt) : null;
        const end = event.endAt ? new Date(event.endAt) : null;
        const todayKey = displayDate(now, { year: 'numeric', month: '2-digit', day: '2-digit' });
        const eventKey = displayDate(start, { year: 'numeric', month: '2-digit', day: '2-digit' });
        const tomorrow = new Date(now.valueOf() + 24 * 60 * 60 * 1000);
        const tomorrowKey = displayDate(tomorrow, { year: 'numeric', month: '2-digit', day: '2-digit' });
        const minutesUntil = start && !Number.isNaN(start.valueOf())
          ? Math.round((start - now) / 60000)
          : null;
        const happeningNow = start && end && start <= now && end > now;
        let relativeLabel = event.allDay ? 'ALL DAY' : 'UP NEXT';
        if (!event.allDay) {
          if (happeningNow) relativeLabel = 'NOW';
          else if (minutesUntil !== null && minutesUntil >= 0 && minutesUntil < 60) relativeLabel = `IN ${minutesUntil} MIN`;
          else if (minutesUntil !== null && minutesUntil >= 60 && minutesUntil < 360) {
            const hours = Math.floor(minutesUntil / 60);
            const minutes = minutesUntil % 60;
            relativeLabel = minutes ? `IN ${hours}H ${minutes}M` : `IN ${hours}H`;
          } else if (eventKey === tomorrowKey) relativeLabel = 'TOMORROW';
        }
        return {
          title: cleanText(event.title, 38) || 'Busy',
          location: cleanText(event.location, 30),
          startAt: event.startAt,
          endAt: event.endAt,
          allDay: Boolean(event.allDay),
          dayLabel: eventKey === todayKey ? 'TODAY' : eventKey === tomorrowKey ? 'TOMORROW' : displayDate(start, { weekday: 'short' }).toUpperCase(),
          timeLabel: event.allDay ? 'ALL DAY' : displayDate(start, { hour: 'numeric', minute: '2-digit' }).replace(' ', '').toUpperCase(),
          relativeLabel,
          urgent: !event.allDay && (happeningNow || (minutesUntil !== null && minutesUntil >= 0 && minutesUntil <= 60)),
        };
      })
    : [];
  const sourceLabels = {
    apple_messages: 'MESSAGES',
    gmail: 'MAIL',
    slack: 'SLACK',
    whatsapp: 'WHATSAPP',
  };
  const timeline = selectTimelineEntries(parsed.timeline, now).map((entry) => {
    const occurredAt = entry.at;
    const actor = cleanDisplayText(cleanSlackMarkup(entry.actor, entry.mentions), 22);
    const summary = cleanDisplayText(cleanSlackMarkup(entry.summary, entry.mentions), 64);
    return {
      occurredAt: entry.occurredAt,
      timeLabel: displayDate(occurredAt, { hour: 'numeric', minute: '2-digit' }).replace(' ', '').toUpperCase(),
      sourceLabel: sourceLabels[entry.source] ?? cleanDisplayText(entry.source, 12).replaceAll('_', ' ').toUpperCase(),
      actor: displayActor(actor),
      summary: summary || 'Activity',
    };
  });

  return {
    protocolVersion: PROTOCOL_VERSION,
    generatedAt: parsed.generatedAt,
    timezone: TIMEZONE,
    stale: false,
    display: {
      dateLabel: displayDate(now, { weekday: 'long', month: 'short', day: 'numeric' }).toUpperCase(),
      updatedLabel: `UPDATED ${displayDate(new Date(parsed.generatedAt), { hour: 'numeric', minute: '2-digit' }).replace(' ', '').toUpperCase()}`,
    },
    calendar,
    timeline,
    // Filled in by the loader once a candidate photo has been fetched and shown
    // to survive quantisation; metadata alone is not worth a slot on the panel.
    photo: photoDisplay(firstPhotoCandidate(parsed)),
    freshness: parsed.freshness ?? {},
  };
}

function photoCandidates(parsed) {
  if (Array.isArray(parsed.photos)) return parsed.photos.filter(Boolean);
  return parsed.photo ? [parsed.photo] : [];
}

function firstPhotoCandidate(parsed) {
  return photoCandidates(parsed)[0] ?? null;
}

// Display-side metadata for a chosen photo. Storage references and signed URLs
// never appear here; the device only learns when the picture was taken.
export function photoDisplay(candidate) {
  const capturedAt = candidate?.capturedAt ? new Date(candidate.capturedAt) : null;
  if (!capturedAt || Number.isNaN(capturedAt.valueOf())) return null;
  return {
    capturedAt: candidate.capturedAt,
    capturedLabel: displayDate(capturedAt, {
      weekday: 'short',
      month: 'short',
      day: 'numeric',
    }).toUpperCase(),
    width: PHOTO_WIDTH,
    height: PHOTO_HEIGHT,
    encoding: 'gxepd7c-4bpp',
    pixels: '',
  };
}

// The six inks an E Ink Spectra 6 panel actually has, in GxEPD2_7C index order.
// Orange is deliberately absent: GDEP073E01 has no orange state, and dithering
// toward it makes the driver emit reserved wire code 0x4, whose rendered result
// is undefined. Indices must stay in this order — the firmware maps them back
// to GxEPD colours positionally.
const PHOTO_PALETTE = [
  [0, 0, 0],
  [255, 255, 255],
  [0, 168, 70],
  [20, 70, 190],
  [220, 35, 35],
  [245, 215, 25],
];

function nearestPaletteColor(red, green, blue) {
  let best = 0;
  let bestDistance = Number.POSITIVE_INFINITY;
  for (let index = 0; index < PHOTO_PALETTE.length; index += 1) {
    const [paletteRed, paletteGreen, paletteBlue] = PHOTO_PALETTE[index];
    const redDelta = red - paletteRed;
    const greenDelta = green - paletteGreen;
    const blueDelta = blue - paletteBlue;
    const distance = 0.30 * redDelta * redDelta
      + 0.59 * greenDelta * greenDelta
      + 0.11 * blueDelta * blueDelta;
    if (distance < bestDistance) {
      best = index;
      bestDistance = distance;
    }
  }
  return best;
}

// Floyd-Steinberg over the six inks. Takes plain top-down RGB triplets so the
// caller decides how pixels were produced -- sharp in the gateway, a synthetic
// bitmap in the simulator's fixture generator, which therefore needs no native
// image dependency.
export function quantizeRgb(rgb, width, height) {
  if (width !== PHOTO_WIDTH || height !== PHOTO_HEIGHT) {
    throw new Error(`Photo must be ${PHOTO_WIDTH}x${PHOTO_HEIGHT}, got ${width}x${height}`);
  }
  if (rgb.length < width * height * 3) throw new Error('Photo pixel buffer is truncated');

  const working = new Float32Array(width * height * 3);
  for (let i = 0; i < width * height * 3; i += 1) working[i] = rgb[i];

  const packed = Buffer.alloc(PHOTO_BYTES, 0x11);
  const spreadError = (x, y, redError, greenError, blueError, weight) => {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    const offset = (y * width + x) * 3;
    working[offset] += redError * weight;
    working[offset + 1] += greenError * weight;
    working[offset + 2] += blueError * weight;
  };
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 3;
      const red = Math.max(0, Math.min(255, working[offset]));
      const green = Math.max(0, Math.min(255, working[offset + 1]));
      const blue = Math.max(0, Math.min(255, working[offset + 2]));
      const color = nearestPaletteColor(red, green, blue);
      const packedOffset = y * width / 2 + Math.floor(x / 2);
      if (x % 2 === 0) packed[packedOffset] = (color << 4) | (packed[packedOffset] & 0x0f);
      else packed[packedOffset] = (packed[packedOffset] & 0xf0) | color;

      const [paletteRed, paletteGreen, paletteBlue] = PHOTO_PALETTE[color];
      const redError = red - paletteRed;
      const greenError = green - paletteGreen;
      const blueError = blue - paletteBlue;
      spreadError(x + 1, y, redError, greenError, blueError, 7 / 16);
      spreadError(x - 1, y + 1, redError, greenError, blueError, 3 / 16);
      spreadError(x, y + 1, redError, greenError, blueError, 5 / 16);
      spreadError(x + 1, y + 1, redError, greenError, blueError, 1 / 16);
    }
  }
  return packed;
}

// Kept for the simulator's fixture generator, which synthesises a BMP rather
// than depending on a native image library.
export function quantizeBmp(bitmap) {
  if (!Buffer.isBuffer(bitmap) || bitmap.length < 54 || bitmap.toString('ascii', 0, 2) !== 'BM') {
    throw new Error('Photo converter returned an invalid BMP');
  }
  const pixelOffset = bitmap.readUInt32LE(10);
  const width = bitmap.readInt32LE(18);
  const signedHeight = bitmap.readInt32LE(22);
  const height = Math.abs(signedHeight);
  const bitsPerPixel = bitmap.readUInt16LE(28);
  const compression = bitmap.readUInt32LE(30);
  if (width !== PHOTO_WIDTH || height !== PHOTO_HEIGHT || bitsPerPixel !== 24 || compression !== 0) {
    throw new Error('Photo converter returned an unsupported BMP layout');
  }
  const rowBytes = Math.ceil(width * 3 / 4) * 4;
  if (pixelOffset + rowBytes * height > bitmap.length) {
    throw new Error('Photo converter returned a truncated BMP');
  }
  const rgb = Buffer.alloc(width * height * 3);
  for (let y = 0; y < height; y += 1) {
    const sourceY = signedHeight < 0 ? y : height - 1 - y;
    for (let x = 0; x < width; x += 1) {
      const source = pixelOffset + sourceY * rowBytes + x * 3;
      const target = (y * width + x) * 3;
      rgb[target] = bitmap[source + 2];
      rgb[target + 1] = bitmap[source + 1];
      rgb[target + 2] = bitmap[source];
    }
  }
  return quantizeRgb(rgb, width, height);
}

// A six-ink panel has no room for a bad candidate. Dark frames collapse to a
// near-solid black blob, blown-out ones to a blank rectangle, and flat ones show
// nothing at all — all three read as "the photo is broken" rather than as a
// picture. Judge the quantised result, since that is what the panel shows.
const PHOTO_MAX_BLACK_SHARE = 0.55;
const PHOTO_MAX_WHITE_SHARE = 0.82;
const PHOTO_MIN_DISTINCT_COLORS = 3;
const PHOTO_COLOR_PRESENCE = 0.02;

export function evaluateQuantizedPhoto(packed) {
  const counts = new Array(PHOTO_PALETTE.length).fill(0);
  for (const byte of packed) {
    const high = byte >> 4;
    const low = byte & 0x0f;
    if (high < counts.length) counts[high] += 1;
    if (low < counts.length) counts[low] += 1;
  }
  const total = packed.length * 2;
  const share = counts.map((count) => count / total);
  const blackShare = share[0];
  const whiteShare = share[1];
  const distinctColors = share.filter((value) => value >= PHOTO_COLOR_PRESENCE).length;

  let reason = '';
  if (blackShare > PHOTO_MAX_BLACK_SHARE) {
    reason = `too dark (${Math.round(blackShare * 100)}% black)`;
  } else if (whiteShare > PHOTO_MAX_WHITE_SHARE) {
    reason = `too washed out (${Math.round(whiteShare * 100)}% white)`;
  } else if (distinctColors < PHOTO_MIN_DISTINCT_COLORS) {
    reason = `too flat (${distinctColors} colours present)`;
  }
  return { ok: reason === '', reason, blackShare, whiteShare, distinctColors };
}

// Centre-crop to the panel aperture and reduce to the six inks. DESIGN.md calls
// for a centre crop rather than a letterbox, so the photo fills the frame.
async function convertPhoto(sourceBytes) {
  const { data, info } = await sharp(sourceBytes, { failOn: 'none' })
    .rotate() // honour EXIF orientation before cropping
    .resize(PHOTO_WIDTH, PHOTO_HEIGHT, { fit: 'cover', position: 'centre' })
    .flatten({ background: '#ffffff' })
    .removeAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });
  return quantizeRgb(data, info.width, info.height);
}

async function loadPhoto(reference, { baseUrl, token, fetchImpl }) {
  const objectResponse = await fetchImpl(`${baseUrl}/api/tools/get_object`, {
    method: 'POST',
    headers: {
      authorization: `Bearer home-display:${token}`,
      'content-type': 'application/json',
    },
    body: JSON.stringify({
      storage_file_id: reference.storageFileId,
      storage_backend: reference.storageBackend,
      storage_key: reference.storageKey,
      account: reference.account,
    }),
    signal: AbortSignal.timeout(12_000),
  });
  if (!objectResponse.ok) throw new Error(`PDW object API returned HTTP ${objectResponse.status}`);
  const objectBody = await objectResponse.json();
  const downloadUrl = objectBody?.data?.download_url;
  if (!downloadUrl) throw new Error('PDW object API returned no download URL');

  const imageResponse = await fetchImpl(downloadUrl, { signal: AbortSignal.timeout(15_000) });
  if (!imageResponse.ok) throw new Error(`Photo download returned HTTP ${imageResponse.status}`);
  const contentLength = Number(imageResponse.headers.get('content-length'));
  if (Number.isFinite(contentLength) && contentLength > MAX_SOURCE_PHOTO_BYTES) {
    throw new Error('Photo download is too large');
  }
  const sourceBytes = Buffer.from(await imageResponse.arrayBuffer());
  if (sourceBytes.length === 0 || sourceBytes.length > MAX_SOURCE_PHOTO_BYTES) {
    throw new Error('Photo download has an invalid size');
  }
  const pixels = await convertPhoto(sourceBytes);
  return {
    width: PHOTO_WIDTH,
    height: PHOTO_HEIGHT,
    encoding: 'gxepd7c-4bpp',
    pixels: pixels.toString('base64'),
    evaluation: evaluateQuantizedPhoto(pixels),
  };
}

// Walks the candidates newest first and keeps the first that survives
// quantisation, so one dark frame does not cost the panel its photo for the
// next half hour.
async function selectPhoto(candidates, context, photoLoader) {
  for (const candidate of candidates) {
    try {
      const rendered = await photoLoader(candidate, context);
      const evaluation = rendered.evaluation ?? { ok: true, reason: '' };
      if (!evaluation.ok) {
        console.error(`[home-display] photo skipped (${candidate.capturedAt}): ${evaluation.reason}`);
        continue;
      }
      const { evaluation: _ignored, ...pixels } = rendered;
      return { ...photoDisplay(candidate), ...pixels };
    } catch (error) {
      console.error(`[home-display] photo candidate failed: ${error.message}`);
    }
  }
  return null;
}

async function readTrimmed(path) {
  const value = (await readFile(path, 'utf8')).trim();
  if (!value) throw new Error(`Required secret is empty: ${path}`);
  return value;
}

// Environment first so a container needs no files on disk; the pdw CLI's config
// file remains the fallback for local development on a workstation. Variable
// names match the pdw CLI's own.
async function readPdwConfig(path) {
  const envUrl = process.env.PDW_API_URL ?? process.env.PDW_BASE_URL ?? '';
  const envToken = process.env.PDW_SECRET_TOKEN ?? process.env.PDW_TOKEN ?? '';
  if (envUrl || envToken) {
    const baseUrl = envUrl.replace(/\/$/, '');
    const token = envToken.trim();
    if (!baseUrl || token.length < 32) {
      throw new Error('PDW_API_URL and PDW_SECRET_TOKEN must both be set and valid');
    }
    return { baseUrl, token };
  }
  const config = JSON.parse(await readFile(path, 'utf8'));
  const baseUrl = String(config.base_url ?? '').replace(/\/$/, '');
  const token = String(config.token ?? '').trim();
  if (!baseUrl || token.length < 32) throw new Error('PDW config is incomplete');
  return { baseUrl, token };
}

async function writeCache(path, dashboard) {
  try {
    await mkdir(dirname(path), { recursive: true, mode: 0o700 });
    const temp = `${path}.${process.pid}.tmp`;
    await writeFile(temp, `${JSON.stringify(dashboard)}\n`, { mode: 0o600 });
    await rename(temp, path);
  } catch (error) {
    // A read-only or missing volume costs the 24-hour fallback, not the render.
    console.error(`[home-display] cache write failed: ${error.message}`);
  }
}

async function readCache(path) {
  try {
    const cached = JSON.parse(await readFile(path, 'utf8'));
    if (cached.protocolVersion !== PROTOCOL_VERSION || cached.health || cached.finance || cached.inbox
        || cached.display?.netWorth || cached.display?.cash) return null;
    const age = Date.now() - new Date(cached.generatedAt).valueOf();
    if (!Number.isFinite(age) || age > STALE_MAX_AGE_MS) return null;
    return cached;
  } catch {
    return null;
  }
}

export function createDashboardLoader(options = {}) {
  const queryPath = options.queryPath ?? DEFAULT_QUERY_PATH;
  const pdwConfigPath = options.pdwConfigPath ?? DEFAULT_PDW_CONFIG;
  const cachePath = options.cachePath ?? DEFAULT_CACHE_PATH;
  const fetchImpl = options.fetchImpl ?? fetch;
  const photoLoader = options.photoLoader ?? loadPhoto;
  let memoryCache = null;
  let inFlight = null;

  async function fetchFresh() {
    const [{ baseUrl, token }, sql] = await Promise.all([
      readPdwConfig(pdwConfigPath),
      readFile(queryPath, 'utf8'),
    ]);
    const response = await fetchImpl(`${baseUrl}/api/tools/sql`, {
      method: 'POST',
      headers: {
        authorization: `Bearer home-display:${token}`,
        'content-type': 'application/json',
      },
      body: JSON.stringify({
        question: 'Render the live personal dashboard on Zach’s reTerminal E1002',
        sql,
        format: 'json',
      }),
      signal: AbortSignal.timeout(12_000),
    });
    if (!response.ok) throw new Error(`PDW API returned HTTP ${response.status}`);
    const body = await response.json();
    const raw = body?.data?.rows?.[0]?.dashboard_json;
    if (!raw) throw new Error('PDW API returned no dashboard row');
    const parsed = JSON.parse(raw);
    const dashboard = normalizeDashboard(parsed);
    const candidates = photoCandidates(parsed);
    if (candidates.length) {
      const chosen = await selectPhoto(candidates, { baseUrl, token, fetchImpl }, photoLoader);
      // Keeping the previous good photo beats showing the placeholder when every
      // candidate is unusable or the object store is briefly unavailable.
      dashboard.photo = chosen ?? (memoryCache?.photo?.pixels ? memoryCache.photo : null);
    }
    memoryCache = dashboard;
    await writeCache(cachePath, dashboard);
    return dashboard;
  }

  return async function loadDashboard({ force = false } = {}) {
    const generated = memoryCache?.generatedAt
      ? new Date(memoryCache.generatedAt).valueOf()
      : 0;
    if (!force && memoryCache && Date.now() - generated < CACHE_TTL_MS) {
      return memoryCache;
    }
    if (inFlight) return inFlight;
    inFlight = fetchFresh()
      .catch(async (error) => {
        const cached = memoryCache ?? await readCache(cachePath);
        if (!cached) throw error;
        return { ...cached, stale: true };
      })
      .finally(() => { inFlight = null; });
    return inFlight;
  };
}

function jsonResponse(response, status, payload) {
  const body = JSON.stringify(payload);
  response.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(body),
    'cache-control': 'private, no-store',
    'x-content-type-options': 'nosniff',
    'referrer-policy': 'no-referrer',
  });
  response.end(body);
}

export function createApp({ token, loadDashboard }) {
  if (token.length < 32) throw new Error('Dashboard token must be at least 32 characters');
  return async (request, response) => {
    try {
      const url = new URL(request.url, 'http://localhost');
      if (url.pathname === '/health') {
        if (request.method !== 'GET') return jsonResponse(response, 405, { error: 'method_not_allowed' });
        return jsonResponse(response, 200, { ok: true, service: 'home-display' });
      }
      if (url.pathname !== '/api/dashboard') {
        return jsonResponse(response, 404, { error: 'not_found' });
      }
      if (request.method !== 'GET') {
        return jsonResponse(response, 405, { error: 'method_not_allowed' });
      }
      const authorization = request.headers.authorization ?? '';
      const supplied = authorization.startsWith('Bearer ') ? authorization.slice(7) : '';
      if (!supplied || !safeEqual(supplied, token)) {
        return jsonResponse(response, 401, { error: 'unauthorized' });
      }
      const dashboard = await loadDashboard({ force: url.searchParams.get('refresh') === '1' });
      return jsonResponse(response, 200, dashboard);
    } catch (error) {
      console.error(`[home-display] request failed: ${error.message}`);
      return jsonResponse(response, 503, { error: 'dashboard_unavailable' });
    }
  };
}

export async function startServer(options = {}) {
  const host = options.host ?? process.env.HOME_DISPLAY_HOST ?? '127.0.0.1';
  const port = Number(options.port ?? process.env.HOME_DISPLAY_PORT ?? 8787);
  const tokenPath = options.tokenPath ?? process.env.HOME_DISPLAY_TOKEN_FILE ?? DEFAULT_TOKEN_PATH;
  const token = options.token
    ?? ((process.env.HOME_DISPLAY_TOKEN ?? '').trim() || await readTrimmed(tokenPath));
  const loadDashboard = options.loadDashboard ?? createDashboardLoader({
    pdwConfigPath: process.env.PDW_CONFIG_PATH ?? DEFAULT_PDW_CONFIG,
    cachePath: process.env.HOME_DISPLAY_CACHE_FILE ?? DEFAULT_CACHE_PATH,
  });
  const server = createServer(createApp({ token, loadDashboard }));
  await new Promise((resolvePromise, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolvePromise);
  });
  console.log(`[home-display] listening on http://${host}:${port}`);
  return server;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  startServer().catch((error) => {
    console.error(`[home-display] fatal: ${error.message}`);
    process.exitCode = 1;
  });
}
