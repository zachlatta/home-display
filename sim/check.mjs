// Renders every fixture plus the two non-dashboard screens and checks the
// results for problems that would only otherwise show up on the physical panel.
//
// Failures (exit 1):
//   * the firmware parser rejects a payload
//   * paged and single-page rendering disagree, meaning the layout depends on
//     where the 40-row page boundaries fall
//   * pixels use a colour the panel has no ink for
//   * the panel is left powered instead of hibernated
//
// Warnings (exit 0): text the device will truncate, and palette colours with
// too little contrast against the background to carry meaning.

import { execFileSync } from 'node:child_process';
import { readdirSync, mkdirSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const SIM_DIR = dirname(fileURLToPath(import.meta.url));
const BINARY = join(SIM_DIR, 'build', 'simulate');
const FIXTURE_DIR = join(SIM_DIR, 'fixtures');
const OUT_DIR = join(SIM_DIR, 'out');
const MIN_CONTRAST = 3.0;

mkdirSync(OUT_DIR, { recursive: true });

function run(args) {
  try {
    return JSON.parse(execFileSync(BINARY, [...args, '--json'], { encoding: 'utf8' }));
  } catch (error) {
    const detail = (error.stderr || error.message).trim();
    return { fatal: detail };
  }
}

const cases = [
  ...readdirSync(FIXTURE_DIR)
    .filter((name) => name.endsWith('.json'))
    .sort()
    .map((name) => {
      const label = basename(name, '.json');
      return { label, args: [join(FIXTURE_DIR, name), '--out', join(OUT_DIR, label)] };
    }),
  { label: 'screen:setup', args: ['--screen', 'setup', '--out', join(OUT_DIR, 'setup')] },
  { label: 'screen:error', args: ['--screen', 'error', '--out', join(OUT_DIR, 'error')] },
];

const failures = [];
const warnings = [];

for (const { label, args } of cases) {
  const report = run(args);
  if (report.fatal) {
    failures.push(`${label}: ${report.fatal}`);
    console.log(`FAIL  ${label.padEnd(14)} ${report.fatal.split('\n')[0]}`);
    continue;
  }

  const problems = [];
  if (!report.pagedMatchesSinglePage) {
    problems.push(`${report.pageMismatchPixels} px differ between paged and single-page render`);
  }
  if (report.unsupportedPixels > 0) {
    const bad = report.colours.filter((colour) => !colour.supported).map((c) => c.name);
    problems.push(`${report.unsupportedPixels} px use unsupported colour(s): ${bad.join(', ')}`);
  }
  if (!report.hibernated) problems.push('panel left powered (no hibernate)');

  for (const problem of problems) failures.push(`${label}: ${problem}`);

  const lowContrast = report.colours.filter(
    (colour) => colour.name !== 'white' && colour.pixels > 0 && colour.contrastVsWhite < MIN_CONTRAST,
  );
  for (const colour of lowContrast) {
    warnings.push(
      `${label}: ${colour.name} is ${colour.contrastVsWhite.toFixed(1)}:1 against the background `
      + `(${colour.pixels} px) — will read as a faint smudge`,
    );
  }
  for (const clip of report.clipped ?? []) {
    warnings.push(`${label}: ${clip.field} truncated to "${clip.rendered}"`);
  }

  const status = problems.length ? 'FAIL' : 'ok  ';
  const summary = report.screen === 'dashboard'
    ? `${report.content.timelineEntries} entries, photo ${report.content.photoDecoded ? 'yes' : 'no '}`
    : `${report.screen} screen`;
  console.log(
    `${status}  ${label.padEnd(14)} ${summary.padEnd(24)} ${report.pages} pages  `
    + `${problems.length ? problems.join('; ') : 'clean'}`,
  );
}

if (warnings.length) {
  console.log('\nwarnings');
  for (const warning of warnings) console.log(`  - ${warning}`);
}

if (failures.length) {
  console.log('\nfailures');
  for (const failure of failures) console.log(`  - ${failure}`);
  console.log(`\n${failures.length} failing, ${warnings.length} warning(s)`);
  process.exit(1);
}

console.log(`\nall ${cases.length} renders clean, ${warnings.length} warning(s)`);
