// GxAlphaToCoverage.h - which draws get alpha-to-coverage (B · a2c-1, 20/10/2026; modernize/render-dlss/SCOPE_a2c.md + PLAN_a2c-1.md).
// One pure rule, read by the translator per draw (it sets the pipeline key's alphaToCoverage byte AND bit 3 of FF spec word 0 from the
// same answer) and unit-tested in gx_ffstate_test. A2C is on iff every condition holds; any one alone turns it off:
//   the session (GXAlphaToCoverage on and the device loaded the a2c shader family); a multisampled colour target; an ORDERED alpha test
//   (LESS / LESSEQUAL / GREATER / GREATEREQUAL) whose ref is not degenerate; blending off; colour writes on; stencil off; a real depth
//   test (ZENABLE and ZFUNC != ALWAYS: the HUD's SC_ATEST_2D is PASS_ALWAYS, and ZENABLE alone is TRUE almost always); not a gx:vel draw.
// Everything else keeps today's hard alpha test, so an off answer is today's draw exactly.
#pragma once

#include <cstdint>

namespace GXRender {

// FF spec word 0 (GxFFW_AlphaTestAndModeArgs) bit 3: the shader's coverage ramp (gx_ff.frag.glsl under GX_LOOK_A2C). Bits 0-2 are the
// compare op (read as 3 bits), so bit 3 is free; it is set only with the key's alphaToCoverage byte, never alone.
enum : uint32_t { GxFFA2CBit = 1u << 3 };

struct GxA2CInputs
{
	bool     session = false;     // GXAlphaToCoverage on AND the a2c module family loaded (creation MSAA >= 2)
	uint32_t samples = 1;         // the colour target's sample count (VkSampleCountFlagBits)
	bool     alphaTest = false;   // D3DRS_ALPHATESTENABLE
	uint32_t alphaFunc = 8;       // D3DRS_ALPHAFUNC (D3DCMP_*)
	uint32_t alphaRef = 0;        // D3DRS_ALPHAREF (the low 8 bits, as GxPackFF packs it)
	bool     blend = false;       // D3DRS_ALPHABLENDENABLE
	uint32_t writeMask = 0xf;     // the resolved colour write mask (key.s.blend.writeMask)
	bool     zEnable = true;      // D3DRS_ZENABLE != 0
	uint32_t zFunc = 4;           // D3DRS_ZFUNC (D3DCMP_*)
	bool     stencil = false;     // D3DRS_STENCILENABLE
	bool     velocityPs = false;  // a gx:vel draw (render-motion-units' own PS has no ramp)
};

// Why A2C is off (or On). Checked in this order; the tests flip one input at a time and expect its own answer.
enum GxA2CWhy : uint8_t {
	GxA2C_On, GxA2C_Session, GxA2C_Samples, GxA2C_AlphaTest, GxA2C_Func, GxA2C_Degenerate, GxA2C_Blend, GxA2C_NoColour,
	GxA2C_Stencil, GxA2C_NoDepthTest, GxA2C_Velocity, GxA2C_WhyCount
};

inline GxA2CWhy GxA2CDecide(const GxA2CInputs& in)
{
	enum : uint32_t { CmpLess = 2, CmpLessEqual = 4, CmpGreater = 5, CmpGreaterEqual = 7, CmpAlways = 8 };
	if (!in.session)
		return GxA2C_Session;
	if (in.samples < 2)
		return GxA2C_Samples;
	if (!in.alphaTest)
		return GxA2C_AlphaTest;
	const uint32_t f = in.alphaFunc, ref = in.alphaRef & 0xffu;
	if (f != CmpLess && f != CmpLessEqual && f != CmpGreater && f != CmpGreaterEqual)
		return GxA2C_Func;   // EQUAL / NOTEQUAL / NEVER / ALWAYS / invalid: no ordered threshold to ramp around
	// the result does not depend on alpha: GE 0 and LE 255 always pass, G 255 and L 0 never do (the ramp would half-cover them)
	if ((f == CmpGreaterEqual && ref == 0) || (f == CmpLessEqual && ref == 255) || (f == CmpGreater && ref == 255) || (f == CmpLess && ref == 0))
		return GxA2C_Degenerate;
	if (in.blend)
		return GxA2C_Blend;
	if ((in.writeMask & 0xfu) == 0)
		return GxA2C_NoColour;   // depth-only / stencil-volume draws
	if (in.stencil)
		return GxA2C_Stencil;    // stencil volumes, the occluded-object pass
	if (!in.zEnable || in.zFunc == CmpAlways)
		return GxA2C_NoDepthTest;   // HUD / UI (SC_ATEST_2D)
	if (in.velocityPs)
		return GxA2C_Velocity;
	return GxA2C_On;
}

inline const char* GxA2CWhyName(GxA2CWhy w)
{
	static const char* const k[GxA2C_WhyCount] = { "on", "session", "samples", "alpha_test", "func", "degenerate", "blend", "no_colour",
		"stencil", "no_depth_test", "velocity" };
	return w < GxA2C_WhyCount ? k[w] : "?";
}

} // namespace GXRender
