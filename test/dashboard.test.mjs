import assert from 'node:assert/strict';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import {
  cleanSlackMarkup,
  createApp,
  createDashboardLoader,
  evaluateQuantizedPhoto,
  isTimelineNoise,
  normalizeDashboard,
  selectTimelineEntries,
} from '../server/dashboard.mjs';

const token = 'test-token-that-is-definitely-longer-than-32-characters';
const raw = {
  protocolVersion: 5,
  generatedAt: '2026-08-18T01:00:00Z',
  calendar: [{ title: 'A  very   long meeting title that needs to be shortened for eink', startAt: '2026-08-18T14:00:00Z', endAt: '2026-08-18T15:00:00Z', allDay: false }],
  timeline: [
    {
      occurredAt: '2026-08-18T12:15:12Z',
      source: 'apple_messages',
      actor: 'Robin',
      summary: 'A recent timeline message',
    },
    {
      occurredAt: '2026-08-18T12:13:49Z',
      source: 'slack',
      actor: 'me',
      summary: 'A recent self message',
    },
  ],
  photos: [{
    photoId: 'ph_recent',
    account: 'private@example.com',
    capturedAt: '2026-08-17T20:41:24-04:00',
    storageBackend: 'object-store',
    storageKey: 'private/photo.jpg',
    storageFileId: 'private-file-id',
  }],
  freshness: {},
};

const goodPixels = () => {
  // An even spread across the six inks passes the suitability check.
  const buffer = Buffer.alloc(270 * 250 / 2);
  for (let i = 0; i < buffer.length; i += 1) buffer[i] = ((i % 6) << 4) | ((i + 3) % 6);
  return buffer;
};

test('normalizes a privacy-safe dashboard with recent photo metadata', () => {
  const value = normalizeDashboard(raw, new Date('2026-08-18T01:00:00Z'));
  assert.equal(value.calendar.length, 1);
  assert.match(value.calendar[0].title, /…$/);
  assert.equal(value.timeline.length, 2);
  // Chronological: the panel reads as a journal of the day, oldest first.
  assert.equal(value.timeline[0].timeLabel, '8:13AM');
  assert.equal(value.timeline[1].timeLabel, '8:15AM');
  assert.equal(value.timeline[1].sourceLabel, 'MESSAGES');
  assert.equal(value.timeline[0].actor, 'You');
  assert.equal(value.timeline[0].summary, 'A recent self message');
  assert.equal('text' in value.timeline[0], false);
  assert.equal(value.calendar[0].dayLabel, 'TOMORROW');
  assert.equal(value.calendar[0].timeLabel, '10:00AM');
  assert.equal(value.calendar[0].relativeLabel, 'TOMORROW');
  assert.equal(value.calendar[0].urgent, false);
  assert.equal(value.photo.capturedLabel, 'MON, AUG 17');
  assert.equal(value.photo.encoding, 'gxepd7c-4bpp');
  assert.equal('health' in value, false);
  assert.equal('finance' in value, false);
  assert.equal('netWorth' in value.display, false);
  assert.equal(JSON.stringify(value).includes('private-file-id'), false);
});

test('slack markup never reaches the panel raw', () => {
  assert.equal(
    cleanSlackMarkup('<@U01EXAMPLE1> could you look', { U01EXAMPLE1: 'jules' }),
    '@jules could you look',
  );
  // Unresolvable ids must not leak as raw identifiers.
  assert.equal(cleanSlackMarkup('thanks <@U0ZZZZZZZ>!'), 'thanks @someone!');
  assert.equal(cleanSlackMarkup('<@U1|preset> hi'), '@preset hi');
  assert.equal(cleanSlackMarkup('see <https://example.com/a/b|the doc>'), 'see the doc');
  assert.equal(cleanSlackMarkup('at <https://good-display.com/x>'), 'at good-display.com');
  assert.equal(cleanSlackMarkup('in <#C123|design> now'), 'in #design now');
  assert.equal(cleanSlackMarkup('ping <!here> please'), 'ping @here please');
  assert.equal(cleanSlackMarkup('a &amp; b *bold*'), 'a & b bold');
});

test('membership and channel administration events are dropped', () => {
  assert.equal(isTimelineNoise('@sam has joined the channel'), true);
  assert.equal(isTimelineNoise('set the channel topic: shipping'), true);
  assert.equal(isTimelineNoise('shared a file'), true);
  assert.equal(isTimelineNoise(''), true);
  assert.equal(isTimelineNoise('Shipped the onboarding fix to prod'), false);
});

test('the timeline spreads across the day instead of the latest burst', () => {
  const now = new Date('2026-08-18T22:00:00-04:00');
  // Two morning events and a dense late-evening burst: taking the most recent
  // six would show only the burst.
  const entries = [
    { occurredAt: '2026-08-18T08:00:00-04:00', source: 'slack', actor: 'Ali', context: 'a', summary: 'morning one' },
    { occurredAt: '2026-08-18T12:00:00-04:00', source: 'slack', actor: 'Bo', context: 'b', summary: 'midday one' },
    ...Array.from({ length: 12 }, (_, index) => ({
      occurredAt: `2026-08-18T21:${String(30 + index).padStart(2, '0')}:00-04:00`,
      source: 'slack',
      actor: `Person${index}`,
      context: `burst${index}`,
      summary: `evening ${index}`,
    })),
  ];
  const selected = selectTimelineEntries(entries, now);
  assert.equal(selected.length, 6);
  assert.equal(selected[0].summary, 'morning one');
  assert.equal(selected[1].summary, 'midday one');
  // Ascending order, and the burst cannot take every slot.
  for (let i = 1; i < selected.length; i += 1) {
    assert.ok(selected[i].at >= selected[i - 1].at, 'entries must be chronological');
  }
  const eveningCount = selected.filter((entry) => entry.summary.startsWith('evening')).length;
  assert.ok(eveningCount <= 4, `expected the evening burst to be capped, got ${eveningCount}`);
});

test('photos that would render as a blob are rejected', () => {
  const allBlack = Buffer.alloc(270 * 250 / 2, 0x00);
  const dark = evaluateQuantizedPhoto(allBlack);
  assert.equal(dark.ok, false);
  assert.match(dark.reason, /too dark/);

  const allWhite = Buffer.alloc(270 * 250 / 2, 0x11);
  const washed = evaluateQuantizedPhoto(allWhite);
  assert.equal(washed.ok, false);
  assert.match(washed.reason, /washed out|flat/);

  assert.equal(evaluateQuantizedPhoto(goodPixels()).ok, true);
});

test('fixed dashboard query does not read health or financial sources', async () => {
  const sql = await readFile(new URL('../server/dashboard-query.sql', import.meta.url), 'utf8');
  assert.doesNotMatch(sql, /base_whoop|base_plaid|marts_finance|net_worth|recovery_score|sleep_performance/i);
  assert.doesNotMatch(sql, /marts_inbox/);
  assert.match(sql, /FROM timeline\.events/);
  assert.match(sql, /priority IN \('self', 'direct'\)/);
  assert.doesNotMatch(sql, /priority IN \([^)]*'cc'/);
  assert.match(sql, /row_number\(\) OVER/);
  assert.match(sql, /context_rank <= 2/);
  assert.match(sql, /LIMIT 60/);
  assert.match(sql, /channel_join/);
  assert.match(sql, /base_slack\.users/);
  assert.match(sql, /marts_photos\.canonical_renditions/);
  // The mention join must only ever read names, never message bodies.
  assert.doesNotMatch(sql, /base_slack\.messages/);
});

test('dashboard endpoint requires its scoped bearer token', async (t) => {
  const server = createServer(createApp({ token, loadDashboard: async () => normalizeDashboard(raw) }));
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  t.after(() => server.close());
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;

  const denied = await fetch(`${base}/api/dashboard`);
  assert.equal(denied.status, 401);
  assert.deepEqual(await denied.json(), { error: 'unauthorized' });

  const wrongMethod = await fetch(`${base}/api/dashboard`, {
    method: 'POST',
    headers: { authorization: `Bearer ${token}` },
  });
  assert.equal(wrongMethod.status, 405);

  const allowed = await fetch(`${base}/api/dashboard`, { headers: { authorization: `Bearer ${token}` } });
  assert.equal(allowed.status, 200);
  assert.equal((await allowed.json()).protocolVersion, 5);
});

test('health endpoint discloses no dashboard data', async (t) => {
  const server = createServer(createApp({ token, loadDashboard: async () => normalizeDashboard(raw) }));
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  t.after(() => server.close());
  const { port } = server.address();
  const response = await fetch(`http://127.0.0.1:${port}/health`);
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { ok: true, service: 'home-display' });
});

test('an unusable photo falls through to the next candidate', async (t) => {
  const directory = await mkdtemp(join(tmpdir(), 'home-display-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const configPath = join(directory, 'pdw.json');
  const queryPath = join(directory, 'query.sql');
  const cachePath = join(directory, 'cache.json');
  await writeFile(configPath, JSON.stringify({ base_url: 'https://pdw.invalid', token: 'x'.repeat(40) }));
  await writeFile(queryPath, 'select 1');

  const twoPhotos = {
    ...raw,
    photos: [
      { ...raw.photos[0], capturedAt: '2026-08-17T23:00:00-04:00', storageKey: 'dark.jpg' },
      { ...raw.photos[0], capturedAt: '2026-08-17T12:00:00-04:00', storageKey: 'good.jpg' },
    ],
  };
  const attempted = [];
  const loader = createDashboardLoader({
    pdwConfigPath: configPath,
    queryPath,
    cachePath,
    photoLoader: async (reference) => {
      attempted.push(reference.storageKey);
      const pixels = reference.storageKey === 'dark.jpg'
        ? Buffer.alloc(270 * 250 / 2, 0x00)
        : goodPixels();
      return {
        width: 270,
        height: 250,
        encoding: 'gxepd7c-4bpp',
        pixels: pixels.toString('base64'),
        evaluation: evaluateQuantizedPhoto(pixels),
      };
    },
    fetchImpl: async () => new Response(JSON.stringify({
      data: { rows: [{ dashboard_json: JSON.stringify(twoPhotos) }] },
    }), { status: 200, headers: { 'content-type': 'application/json' } }),
  });

  const value = await loader({ force: true });
  assert.deepEqual(attempted, ['dark.jpg', 'good.jpg']);
  // The chosen photo's own capture time must be the one shown.
  assert.equal(value.photo.capturedAt, '2026-08-17T12:00:00-04:00');
  assert.ok(value.photo.pixels.length > 0);
});

test('a photo download URL off the warehouse origin is refused', async (t) => {
  const directory = await mkdtemp(join(tmpdir(), 'home-display-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const configPath = join(directory, 'pdw.json');
  const queryPath = join(directory, 'query.sql');
  await writeFile(configPath, JSON.stringify({ base_url: 'https://pdw.invalid', token: 'x'.repeat(40) }));
  await writeFile(queryPath, 'select 1');

  // A PDW that has been compromised, or is simply wrong, must not be able to
  // steer the gateway at an internal address on the container network.
  const hostile = [
    'http://169.254.169.254/latest/meta-data/',
    'http://127.0.0.1:8787/api/dashboard',
    'https://attacker.example.com/x.jpg',
  ];
  const fetched = [];
  for (const downloadUrl of hostile) {
    const loader = createDashboardLoader({
      pdwConfigPath: configPath,
      queryPath,
      cachePath: join(directory, `cache-${fetched.length}.json`),
      minForcedRefreshMs: 0,
      fetchImpl: async (url, init) => {
        if (String(url).endsWith('/api/tools/sql')) {
          return new Response(JSON.stringify({ data: { rows: [{ dashboard_json: JSON.stringify(raw) }] } }), { status: 200 });
        }
        if (String(url).endsWith('/api/tools/get_object')) {
          return new Response(JSON.stringify({ data: { download_url: downloadUrl } }), { status: 200 });
        }
        fetched.push(String(url));
        return new Response(Buffer.alloc(16), { status: 200 });
      },
    });
    const value = await loader({ force: true });
    assert.equal(value.photo, null, `expected no photo for ${downloadUrl}`);
  }
  assert.deepEqual(fetched, [], 'the gateway must not have fetched any disallowed URL');
});

test('a non-JPEG photo body is refused', async (t) => {
  const directory = await mkdtemp(join(tmpdir(), 'home-display-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const configPath = join(directory, 'pdw.json');
  const queryPath = join(directory, 'query.sql');
  await writeFile(configPath, JSON.stringify({ base_url: 'https://pdw.invalid', token: 'x'.repeat(40) }));
  await writeFile(queryPath, 'select 1');

  const loader = createDashboardLoader({
    pdwConfigPath: configPath,
    queryPath,
    cachePath: join(directory, 'cache.json'),
    minForcedRefreshMs: 0,
    fetchImpl: async (url) => {
      if (String(url).endsWith('/api/tools/sql')) {
        return new Response(JSON.stringify({ data: { rows: [{ dashboard_json: JSON.stringify(raw) }] } }), { status: 200 });
      }
      if (String(url).endsWith('/api/tools/get_object')) {
        return new Response(JSON.stringify({ data: { download_url: 'https://pdw.invalid/object/1' } }), { status: 200 });
      }
      // An SVG would otherwise reach librsvg inside libvips.
      return new Response(Buffer.from('<svg xmlns="http://www.w3.org/2000/svg"/>'), { status: 200 });
    },
  });
  const value = await loader({ force: true });
  assert.equal(value.photo, null, 'a non-JPEG body must not reach the decoder');
});

test('loader serves the last good payload when PDW is temporarily unavailable', async (t) => {
  const directory = await mkdtemp(join(tmpdir(), 'home-display-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const configPath = join(directory, 'pdw.json');
  const queryPath = join(directory, 'query.sql');
  const cachePath = join(directory, 'cache.json');
  await writeFile(configPath, JSON.stringify({ base_url: 'https://pdw.invalid', token: 'x'.repeat(40) }));
  await writeFile(queryPath, 'select 1');
  let available = true;
  const loader = createDashboardLoader({
    pdwConfigPath: configPath,
    queryPath,
    cachePath,
    minForcedRefreshMs: 0, // exercise the failure path, not the amplification guard
    photoLoader: async () => ({
      width: 270,
      height: 250,
      encoding: 'gxepd7c-4bpp',
      pixels: goodPixels().toString('base64'),
      evaluation: { ok: true, reason: '' },
    }),
    fetchImpl: async () => {
      if (!available) throw new Error('offline');
      return new Response(JSON.stringify({
        data: { rows: [{ dashboard_json: JSON.stringify(raw) }] },
      }), { status: 200, headers: { 'content-type': 'application/json' } });
    },
  });

  const fresh = await loader({ force: true });
  assert.equal(fresh.stale, false);
  available = false;
  const fallback = await loader({ force: true });
  assert.equal(fallback.stale, true);
  assert.equal(fallback.protocolVersion, 5);
  assert.equal('health' in fallback, false);
  assert.equal('finance' in fallback, false);
});
