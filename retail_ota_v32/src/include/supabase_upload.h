#pragma once

// v28 Supabase REST insert over HTTPS; duplicates of a reading_id are ignored by the server.

// Up to 3 attempts, transient errors only. Records the HTTP status in retained.previousHttp.
// True on a 2xx response.
bool supabaseUpload(const char* payload, int length);
