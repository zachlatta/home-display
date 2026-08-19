# Dashboard design: jobs to be done

## Evidence

This design is grounded in the Personal Data Warehouse and prior dashboard work, not a
generic collection of widgets.

- In a prior jobs-to-be-done query, Zach explicitly narrowed the product to one job:
  **“Show me what I actually did today.”** He preferred a truthful, scannable journal over
  goals, scores, recommendations, and vanity metrics.
- The overall PDW timeline is high-volume. Broad copied activity is dominated by ambient
  channel traffic, so raw `cc` recency is not a useful proxy for Zach's day. The display
  uses only `self` and `direct` entries, within the current New York day, and excludes
  system runs and sources already represented elsewhere. Repeated events in the same
  source/context are collapsed to the latest entry, turning message bursts into readable
  activity blocks rather than letting one conversation occupy the screen.
- The near-term calendar is dense enough that one clear transition is useful, but a wall
  of appointments competes with the primary job.
- A recent camera photo gives the passive display emotional value even when no action is
  needed. Six inks are unforgiving, though: a dark or flat frame reduces to a shapeless
  blob that reads as a fault rather than a picture, so candidates are quantised and
  measured before one is chosen.

The evidence was checked on 2026-08-18. PDW is a synced warehouse, so freshness and the
current query are more authoritative than these notes.

## Jobs

### Primary job

**When I glance at the display during the day, help me reconstruct what has actually
happened so far, so I feel oriented without opening an app.**

The left two-thirds of the screen is therefore a compact chronological timeline, not counts
or scores. Time, source, actor, and a short human-readable summary are visible at a glance,
oldest first, so the column reads like a journal entry rather than an inbox.

Recency alone does not satisfy this job. Selecting the six most recent entries collapsed
the panel into whichever conversation happened last — measured at 11pm, all six rows
covered a thirteen-minute window. The gateway therefore divides the elapsed part of the day
into as many buckets as there are rows, takes one representative from each, and backfills
whichever remaining entry sits furthest in time from everything already chosen. Source
markup, membership events, and bot traffic are removed before selection, because a row
spent on "@zrl has joined the channel" is a row not spent on the day.

### Supporting job: transition

**When I am between things, tell me the one commitment I am moving toward next, so I can
shift attention at the right time.**

Only the next timed calendar event appears (with an all-day item as fallback). Its relative
time is visually louder than its metadata; urgent transitions use red and later ones use green.

### Supporting job: belonging

**When the display is ambient, let it feel like part of my home rather than an operations
console.**

A recent real camera photo occupies the right side. It is deliberately larger than status
metadata, center-cropped rather than letterboxed, and rendered in the panel's native palette.

### Trust job

**When I rely on the display, make it obvious that it is current and private.**

The header carries the update time or a visible cached state. Health, financial metrics,
raw notification counts, signed photo URLs, and storage references never enter the device
payload.

## Visual system

- One dominant hierarchy: today's story, recent moment, next transition.
- Black and white carry all essential information for maximum contrast on reflective
  e-paper. Color is reserved for the masthead signature, source nodes, and urgent accents;
  meaning never depends on low-contrast colored text.
- A compact sans-serif system keeps labels, times, and summaries crisp at the panel's
  resolution and spends less space on decoration.
- Source-colored timeline nodes make scanning faster without adding icons the bundled
  fonts cannot render reliably; their black outline keeps even yellow legible.
- Six timeline entries, one photo, and one next event are deliberate limits. At most two
  entries from one conversation can appear, keeping the view dense without letting a single
  message burst take over. The display is an information radiator, not an inbox.

## Non-jobs

- Measuring self-worth through health or financial scores.
- Reproducing an inbox or unread-count dashboard.
- Showing everything the warehouse knows.
- Asking the e-paper screen to become interactive software.
