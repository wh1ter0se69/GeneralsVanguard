// GeneralsX @feature D · rw3-X-2 13/10/2026 A23-B: the EXEC digest hook - TEST-ONLY (perf-audit/DESIGN_A23_wide-ring.md v8.1 §7.1).
//
// Every EXEC half - on gx-native-cs, inline (GX_NATIVE_CS=0) or inside an S body - reports itself at its top, in the order it
// RUNS: (op, canonical arguments, payload bytes). Both modes run the same halves, so a correct worker gives the same stream; a
// reorder, a payload taken late or a dropped half changes it. Front-executed creations go to a separate CREATION LEDGER in front
// order (their physical time moves with a held worker). Null sink = nothing recorded (the product: one relaxed load per half).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace GXRender { namespace Dev {

enum class GxCsOp : uint16_t
{
	RenderState = 1, TextureStageState, Transform, Viewport, Material, Light, LightEnable, Texture, Stream, Indices,
	CreateVs, CreatePs, SetVs, SetPs, DeleteVs, DeletePs, VsConst, PsConst, Draw, DrawIndexed, DrawUP, Clear, SetTarget,
	Rename, Upload, UploadRect, SetLod, Destroy, CopyGpu, CopyHost, FlushActed, Present, PresentLost, StreamUnbind,
	ShadowFrame, ShadowTags,  // S7 (D · look-shadowmaps): IGxShadowMap's commands
	ShadowOwn,                // S7 A41 (D · look-shadowmaps-3): SetOwnLight
	LogicFrame                // S7 A92 (E · look-shadowmaps-9): testSetLogicFrame, TEST-ONLY (GX_GPUTIME_DUMP)
};

// An argument: a value, or a device object (canonicalised by the sink: numbered by first appearance, a new number after the
// object at that address was destroyed - a reused address is a new object).
struct GxCsArg
{
	uint64_t v;
	bool object;
	GxCsArg(uint64_t x) : v(x), object(false) {}
	GxCsArg(const void* p) : v(uint64_t(reinterpret_cast<uintptr_t>(p))), object(true) {}
};
inline GxCsArg csBits(float f)
{
	uint32_t u;
	static_assert(sizeof(u) == sizeof(f), "float is 32-bit");
	std::memcpy(&u, &f, 4);
	return GxCsArg(uint64_t(u));
}

// Payload bytes as the exec half reads them: `rows` rows of `rowBytes`, `pitch` apart (a flat payload: rows 1). p may be null.
struct GxCsBytes
{
	const void* p = nullptr;
	size_t rowBytes = 0;
	uint32_t rows = 0;
	size_t pitch = 0;
};

class GxCsExecSink
{
public:
	virtual ~GxCsExecSink() = default;
	virtual void exec(GxCsOp op, const GxCsArg* args, size_t n, const GxCsBytes* bytes) = 0;
	// the object is gone after this exec: its address may come back as a new object
	virtual void destroyed(const void* object) = 0;
	// front: a creation, in front order; position = exec halves dispatched before it (both modes count the same)
	virtual void created(uint64_t position, uint32_t kind, const GxCsArg* args, size_t n) = 0;
	// an image leaves the translator's inputs (GxNativeDevice::forgetImage, in exec order): for the translator-input digest's
	// identities only (Rec::g_trInputSink); the EXEC stream never names an image
	virtual void retiredImage(const void*) {}
};

extern std::atomic<GxCsExecSink*> g_csExecSink;

}} // namespace GXRender::Dev
