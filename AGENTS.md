# Agent instructions

## This repository is public

`github.com/zachlatta/home-display` is a **public** repository. Anything committed
here is visible to the world, permanently, and is assumed to be scraped within
minutes of being pushed. Treat every commit as an irreversible publication.

That matters more than usual here, because this project's whole purpose is to
render Zach's private data — messages, calendar, photos — onto a screen. The code
is public; the data it moves must never be.

## Read everything before pushing to main

**Before any push to `main`, read the full diff — every file, every line — and
confirm no secret and no real personal data is in it.** Not a skim of the file
list. The actual content.

```sh
git diff --stat origin/main..HEAD     # what changed
git diff origin/main..HEAD            # read this in full
npm run audit:public                  # mechanical backstop, not a substitute
```

`scripts/audit-public.sh` catches the obvious cases. It cannot catch a real
person's name in a fixture or a paraphrased private message, so it is a floor,
not a ceiling. If the diff is too large to read, it is too large to push.

### Never commit

- Credentials of any kind: bearer tokens, API keys, private keys, passwords,
  recovery codes, cookies, session identifiers.
- `include/secrets.h`, `.secrets/`, `.state/`, `.env` — all gitignored, and that
  gitignore is load-bearing. Do not add exceptions to it.
- Wi-Fi SSIDs and passphrases.
- Internal hostnames, tailnet names, private IP addresses, or anything that maps
  the network topology.

### Never commit, and easier to get wrong

- **Real message content, real names, or real identifiers in fixtures and
  tests.** Every person and every message in `sim/fixtures/` and
  `test/` must be invented. This has already gone wrong once: fixtures were
  seeded from a live payload during development and captured real contacts'
  names, verbatim private messages, and real Slack user IDs. It was caught before
  the first push. It would not have been recoverable afterwards.
- Never generate a fixture from a live payload. `sim/make-fixtures.mjs` builds
  them from invented data on purpose — extend that, do not paste in real output.
- Renders under `sim/out/` are gitignored because they may show real data. Do not
  commit a screenshot of the live display.

### Also worth a second look

- `server/dashboard-query.sql` names real warehouse schemas and tables. That is
  intentional and acceptable — it is structure, not data — but never add a query
  that embeds a literal account name, user ID, or team ID.
- Log lines and error messages must not echo payload content.

## Deployment

The gateway runs on Coolify. `server/` is the only thing in the runtime image;
`src/` is ESP32 firmware and `sim/` is the host simulator, both build-time only.
All configuration is environment variables — see `README.md`. Never read
credentials from a file in production.

Gateway, serial bridge, and firmware share a wire protocol version. Changing the
payload shape means bumping `PROTOCOL_VERSION` in all three of
`server/dashboard.mjs`, `server/serial_bridge.py`, and `src/dashboard_view.h`,
and deploying them together — the device refuses any other version.

## Verification

Run all three before pushing. They need no hardware.

```sh
npm test              # gateway logic, payload shape, security properties
make -C sim check     # renders every fixture through the real firmware layout
pio run -e reterminal_e1002   # firmware still compiles (needs include/secrets.h)
```

`make -C sim check` is the one that catches display bugs: it compiles the actual
firmware layout code, so clipping, page-boundary, and unsupported-colour problems
fail the build instead of reaching the panel.

## Git

- Conventional, imperative commit subjects. Explain *why* in the body.
- Treat published `main` as immutable. Correct mistakes with a new commit; never
  amend, rebase, or force-push once pushed.
- If a secret ever does reach `main`, rotating the credential comes first.
  Rewriting history second, and it does not undo the exposure.
