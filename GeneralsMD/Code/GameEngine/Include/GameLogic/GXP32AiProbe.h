// Shadow counters and fail arms for the script-condition cache, the trigger cache, the
// sequential-script group reissue. They exist only in GX_TEST_INSTRUMENTS builds; a shipping build gets empty macros.
// Env: GX_P32_AI=1 recomputes behind every cache hit and compares; GX_P32_BREAK=version|group|trigger breaks one fix on purpose.
#pragma once

#if defined(GX_TEST_INSTRUMENTS)
struct GXP32AiCounters
{
	UnsignedInt64 baseHit, skip, recompute, trigCalls, trigHit, groupReissue, groupRebuild, mismatch;
};
extern GXP32AiCounters TheGXP32Ai;
enum { GX_P32_BREAK_NONE, GX_P32_BREAK_VERSION, GX_P32_BREAK_GROUP, GX_P32_BREAK_TRIGGER };
Bool gxP32AiOn();
Int gxP32Break();
void gxP32NoteMismatch(const char *what, UnsignedInt frame);
void gxP32AiTick(UnsignedInt frame);
#define GX_P32_COUNT(field) do { if (gxP32AiOn()) TheGXP32Ai.field++; } while (0)
#else
#define GX_P32_COUNT(field) do { } while (0)
#endif
