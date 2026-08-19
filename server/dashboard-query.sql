WITH bounds AS (
  SELECT now() AS now_utc,
         ((now() AT TIME ZONE 'America/New_York')::date::timestamp
           AT TIME ZONE 'America/New_York') AS today_start,
         (((now() AT TIME ZONE 'America/New_York')::date + 2)::timestamp
           AT TIME ZONE 'America/New_York') AS day_after_tomorrow
), deduped_calendar AS (
  SELECT DISTINCT ON (coalesce(nullif(i_cal_uid, ''), event_id), start_at)
         summary, location, start_at, end_at,
         (is_all_day = 1 OR end_at - start_at >= interval '18 hours') AS all_day
  FROM base_google_calendar.events, bounds
  WHERE is_deleted = 0
    AND status <> 'cancelled'
    AND end_at >= bounds.now_utc
    AND start_at < bounds.day_after_tomorrow
  ORDER BY coalesce(nullif(i_cal_uid, ''), event_id), start_at, updated_at DESC
), upcoming_calendar AS (
  SELECT summary, location, start_at, end_at, all_day
  FROM deduped_calendar
  ORDER BY all_day, start_at
  LIMIT 1
), recent_timeline_candidates AS (
  SELECT event_ts, source, actor, context,
         coalesce(nullif(title, ''), nullif(snippet, '')) AS summary,
         row_number() OVER (
           PARTITION BY source, coalesce(nullif(context, ''), nullif(actor, ''), source)
           ORDER BY event_ts DESC, seq DESC
         ) AS context_rank
  FROM timeline.events, bounds
  WHERE priority IN ('self', 'direct')
    AND event_ts >= bounds.today_start
    AND event_ts <= now()
    AND source NOT IN ('calendar', 'photos', 'finance', 'whoop', 'warehouse')
    AND kind <> 'agent_session'
    AND coalesce(nullif(title, ''), nullif(snippet, '')) IS NOT NULL
    -- Membership and channel-administration events pass the self/direct filter
    -- because Zach triggered them, but they describe Slack, not his day. The
    -- subtype is authoritative, unlike matching on message text.
    AND coalesce(metadata ->> 'subtype', '') NOT IN (
      'channel_join', 'channel_leave', 'group_join', 'group_leave',
      'channel_topic', 'channel_purpose', 'channel_name',
      'channel_archive', 'channel_unarchive', 'channel_posting_permissions',
      'pinned_item', 'unpinned_item', 'tombstone', 'reminder_add',
      'tabbed_canvas_updated', 'canvas_sharing_message',
      'bot_message', 'slackbot_response'
    )
    AND coalesce((metadata ->> 'bot')::boolean, false) = false
), timeline_pool AS (
  -- A generous candidate pool: the gateway spreads these across the elapsed
  -- part of the day rather than taking the most recent handful, so it needs
  -- more rows than the six that reach the panel.
  SELECT event_ts, source, actor, context, summary
  FROM recent_timeline_candidates
  WHERE context_rank <= 2
  ORDER BY event_ts DESC
  LIMIT 60
), timeline_resolved AS (
  SELECT p.event_ts, p.source, p.actor, p.context, p.summary,
         -- Slack embeds mentions as <@U…>. Unresolved they are unreadable, so
         -- hand the gateway an id -> name map for whatever this row references.
         (
           SELECT json_object_agg(found.user_id, found.name)
           FROM (
             SELECT DISTINCT mention.match[1] AS user_id, resolved.name
             FROM regexp_matches(p.summary, '<@(U[A-Z0-9]+)', 'g') AS mention(match)
             JOIN LATERAL (
               SELECT coalesce(nullif(u.display_name, ''),
                               nullif(u.real_name, ''),
                               u.name) AS name
               FROM base_slack.users u
               WHERE u.user_id = mention.match[1]
               ORDER BY u.is_deleted, u.synced_at DESC
               LIMIT 1
             ) AS resolved ON true
           ) AS found
         ) AS mentions
  FROM timeline_pool p
), recent_photos AS (
  -- Several candidates, newest first. Dark or flat images become an
  -- undifferentiated blob on a six-ink panel, so the gateway quantises each in
  -- turn and keeps the first one that survives.
  SELECT r.account, r.capture_ts,
         r.storage_backend, r.storage_key, r.storage_file_id
  FROM marts_photos.canonical_renditions r
  JOIN marts_photos.photos p USING (photo_id)
  WHERE p.kind = 'image'
    AND r.mime_type = 'image/jpeg'
    AND nullif(p.camera_make, '') IS NOT NULL
    AND nullif(r.storage_file_id, '') IS NOT NULL
  ORDER BY r.capture_ts DESC NULLS LAST
  LIMIT 6
), source_freshness AS (
  SELECT json_object_agg(table_schema || '.' || table_name,
                         json_build_object(
                           'ageSeconds', data_age_seconds,
                           'lastWriteAt', last_write_at,
                           'status', probe_status)) AS value
  FROM marts_ops.table_freshness
  WHERE (table_schema, table_name) IN (
    ('base_google_calendar', 'events'),
    ('base_gmail', 'messages'),
    ('base_slack', 'messages'),
    ('base_apple_photos', 'files')
  )
)
SELECT json_build_object(
  'protocolVersion', 5,
  'generatedAt', now(),
  'timezone', 'America/New_York',
  'calendar', coalesce((
    SELECT json_agg(json_build_object(
      'title', summary,
      'location', location,
      'startAt', start_at,
      'endAt', end_at,
      'allDay', all_day
    ) ORDER BY start_at)
    FROM upcoming_calendar
  ), '[]'::json),
  'timeline', coalesce((
    SELECT json_agg(json_build_object(
      'occurredAt', event_ts,
      'source', source,
      'actor', actor,
      'context', context,
      'summary', summary,
      'mentions', mentions
    ) ORDER BY event_ts DESC)
    FROM timeline_resolved
  ), '[]'::json),
  'photos', coalesce((
    SELECT json_agg(json_build_object(
      'account', account,
      'capturedAt', capture_ts,
      'storageBackend', storage_backend,
      'storageKey', storage_key,
      'storageFileId', storage_file_id
    ) ORDER BY capture_ts DESC)
    FROM recent_photos
  ), '[]'::json),
  'freshness', (SELECT value FROM source_freshness)
)::text AS dashboard_json;
