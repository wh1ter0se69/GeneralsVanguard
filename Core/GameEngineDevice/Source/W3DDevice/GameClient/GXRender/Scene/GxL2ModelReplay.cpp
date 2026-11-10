/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX: WS-G Task 5: the replay driver - one flush's `=2` call log through the device-less model, with
// C6's call-level comparison (MODEL_CALL, CONTRACT_L2 r9b) built in (GxL2Model.h replay()). Engine-free.

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Model.h"

#include <cstdio>
#include <unordered_map>
#include <vector>

namespace gxl2m {

namespace {

struct Inv { unsigned op, a, b; uint64_t value; bool sent; };

struct Pred : CallSink {
	std::vector<Inv> v;
	bool on = true;
	void invoked(unsigned op, unsigned a, unsigned b, uint64_t value, bool sent) override { if (on) v.push_back({ op, a, b, value, sent }); }
};

bool isLow(uint16_t op) { return op >= gxlog::RS && op < gxlog::ENTRY; }
bool isInput(const gxlog::Record &r) { return r.bracket == gxlog::BR_TEXTURE || r.bracket == gxlog::BR_MATERIAL; }

uint64_t u64At(const gxlog::Log &L, const gxlog::Record &r) { const uint64_t *p = L.get<uint64_t>(r.payload); return p ? *p : 0; }

// A logged low-level record as the invocation the model reports for it.
Inv toInv(const gxlog::Log &L, const gxlog::Record &r)
{
	const bool sent = (r.flags & gxlog::SENT) != 0;
	switch (r.op) {
	case gxlog::RS: return { C_RS, r.a, 0, r.b, sent };
	case gxlog::TSS: return { C_TSS, r.a >> 16, r.a & 0xFFFFu, r.b, sent };
	case gxlog::DX8_TEXTURE: return { C_TEXTURE, r.a, 0, u64At(L, r), sent };
	case gxlog::DX8_MATERIAL: { const Material *m = L.get<Material>(r.payload); return { C_MATERIAL, 0, 0, m ? payloadHash(m, sizeof(*m)) : 0, sent }; }
	case gxlog::DX8_LIGHT: { const Light *l = r.b ? L.get<Light>(r.payload) : nullptr; return { C_LIGHT, r.a, r.b, l ? payloadHash(l, sizeof(*l)) : 0, sent }; }
	case gxlog::DX8_XFORM: { const Mat4 *m = L.get<Mat4>(r.payload); return { C_XFORM, r.a, 0, m ? payloadHash(m, sizeof(*m)) : 0, sent }; }
	case gxlog::STREAM: return { C_STREAM, r.a, r.b, u64At(L, r), true };
	case gxlog::INDICES: return { C_INDICES, 0, r.b, u64At(L, r), true };
	case gxlog::VSHADER: return { C_VSHADER, r.a, 0, 0, true };
	}
	return { 0, 0, 0, 0, false };
}

// Replays a logged low-level record through the model's send function (its own invocation goes to s.sink).
void replayLow(State &s, const gxlog::Log &L, const gxlog::Record &r)
{
	switch (r.op) {
	case gxlog::RS: sendRS(s, r.a, r.b); break;
	case gxlog::TSS: sendTSS(s, r.a >> 16, r.a & 0xFFFFu, r.b); break;
	case gxlog::DX8_TEXTURE: sendTexture(s, r.a, u64At(L, r)); break;
	case gxlog::DX8_MATERIAL: if (const Material *m = L.get<Material>(r.payload)) sendMaterial(s, *m); break;
	case gxlog::DX8_LIGHT: sendLight(s, (int)r.a, r.b ? L.get<Light>(r.payload) : nullptr); break;
	case gxlog::DX8_XFORM: if (const Mat4 *m = L.get<Mat4>(r.payload)) sendXform(s, r.a, *m); break;
	case gxlog::XFORM_RAW: if (const Mat4 *m = L.get<Mat4>(r.payload)) setTransform(s, r.a, *m); break;	// state only, no call compare
	default: break;	// STREAM / INDICES / VSHADER occur only inside an Apply, which the model predicts itself
	}
}

bool sameInv(const Inv &p, const Inv &l)
{
#if GX_L2M_MUTANT == 18
	return p.op == l.op && p.a == l.a && p.b == l.b && p.value == l.value;	// M18: the sent / skipped tag ignored
#else
	return p.op == l.op && p.a == l.a && p.b == l.b && p.value == l.value && p.sent == l.sent;
#endif
}

void compare(const gxlog::Log &L, const std::vector<Inv> &pred, const std::vector<const gxlog::Record *> &logged,
	const gxlog::Record &parent, ReplayHooks &h)
{
	const size_t n = pred.size() > logged.size() ? pred.size() : logged.size();
	for (size_t k = 0; k < n; ++k) {
		if (k >= pred.size()) h.callMismatch(*logged[k], "logged invocation the model did not predict");
		else if (k >= logged.size()) h.callMismatch(parent, "predicted invocation not logged");
		else if (!sameInv(pred[k], toInv(L, *logged[k]))) h.callMismatch(*logged[k], "invocation differs");
	}
}

// One input Apply as the log holds it: which bracket of which classic entry's Apply, for which object, and the Apply's record range
// (C5's boundary replay takes its input brackets from these - PLAN_model T4).
struct Src { uint32_t seq; uint8_t bracket; int stage; uint64_t obj; size_t b, e; };

// An Apply's logged INPUT brackets (texture / material Apply outputs) between b and e.
struct In : Inputs {
	const gxlog::Log &L; size_t b, e; ReplayHooks &h; Pred &p;
	std::vector<char> used;
	std::vector<Src> *collect = nullptr; uint32_t seq = 0;	// C5: every input taken, with its object
	In(const gxlog::Log &l, size_t b_, size_t e_, ReplayHooks &h_, Pred &p_) : L(l), b(b_), e(e_), h(h_), p(p_), used(e_ > b_ ? e_ - b_ : 0, 0) {}
	// The input's sends keep their logged VALUES; the model's cache decision for each (sent / skipped) must agree with the logged tag
	// and anything but a low-level record inside an input bracket is reported, not dropped (#1).
	void take(State &s, uint8_t bracket, int stage, uint64_t obj, const char *missing)
	{
		bool any = false;
		for (size_t i = b; i < e; ++i) {
			const gxlog::Record &r = L.rec[i];
			if (r.bracket != bracket || (bracket == gxlog::BR_TEXTURE && r.stage != stage)) continue;
			used[i - b] = 1;
			if (r.op == gxlog::DEVICE_READ || r.op == gxlog::RESOURCE || r.op == gxlog::RAW_ACCESS) continue;	// provenance: harmless, or the
			// outer scan's UNKNOWN (RAW_ACCESS) - not an input send
			any = true;
			if (!isLow(r.op)) { h.callMismatch(r, "a high-level record inside an input bracket"); continue; }
			const size_t before = p.v.size();
			replayLow(s, L, r);
			if (r.op != gxlog::XFORM_RAW) {
				const bool predictedSent = p.v.size() > before && p.v.back().sent;
				if (predictedSent != ((r.flags & gxlog::SENT) != 0)) h.callMismatch(r, "input invocation: the cache decision differs");
			}
			p.v.resize(before);	// the input's invocations are not predictions of the parent
		}
		if (!any) h.callMismatch(L.rec[b > 0 ? b - 1 : 0], missing);
		else if (collect) collect->push_back({ seq, bracket, stage, obj, b, e });
	}
	void textureApply(State &s, int stage, uint64_t tex) override { take(s, gxlog::BR_TEXTURE, stage, tex, "model applied a texture stage, none logged"); }
	void materialApply(State &s, uint64_t m) override { take(s, gxlog::BR_MATERIAL, 0, m, "model applied the material, none logged"); }
	void finish()
	{
#if GX_L2M_MUTANT != 21
		for (size_t i = b; i < e; ++i)
			if (isInput(L.rec[i]) && !used[i - b] && L.rec[i].op != gxlog::DEVICE_READ && L.rec[i].op != gxlog::RESOURCE
				&& L.rec[i].op != gxlog::RAW_ACCESS)
				h.callMismatch(L.rec[i], "logged input Apply the model did not make");
#endif
	}
};

size_t applyEnd(const gxlog::Log &L, size_t begin, size_t e)
{
	for (size_t j = begin + 1; j < e; ++j)
		if (L.rec[j].op == gxlog::APPLY_END) return j;
	return e;
}

// The NESTED, non-input low-level records between b and e: what the model's port must have predicted.
std::vector<const gxlog::Record *> nestedIn(const gxlog::Log &L, size_t b, size_t e)
{
	std::vector<const gxlog::Record *> v;
	for (size_t i = b; i < e; ++i)
		if (isLow(L.rec[i].op) && L.rec[i].op != gxlog::XFORM_RAW && (L.rec[i].flags & gxlog::NESTED) && !isInput(L.rec[i])) v.push_back(&L.rec[i]);
	return v;
}

// Replays a log one top-level UNIT at a time: a direct low-level call; a draw or an Apply with its Apply block; or a high-level setter with
// the NESTED invocations logged under it. A unit never reaches past `e`, so a caller can replay part of a log (C5 steps two States
// through the same records).
struct Stepper {
	State &s; const gxlog::Log &L; ReplayHooks &h; Pred p; CallSink *const saved;
	std::vector<Src> *collect = nullptr;	// C5: each input Apply taken, with its object
	Stepper(State &s_, const gxlog::Log &l, ReplayHooks &h_) : s(s_), L(l), h(h_), saved(s_.sink) { s.sink = &p; }
	~Stepper() { s.sink = saved; }
	size_t step(size_t i, size_t n);	// the unit at i; returns the index after it (<= n)
};

size_t Stepper::step(size_t i, const size_t n)
{
	const gxlog::Record &r = L.rec[i];
	p.v.clear();
	if (isLow(r.op)) {	// a DIRECT low-level call (engine code calling the cached setter itself)
		if (r.flags & gxlog::NESTED) h.callMismatch(r, "nested invocation outside its parent");
		replayLow(s, L, r);
		if (r.op != gxlog::XFORM_RAW) compare(L, p.v, std::vector<const gxlog::Record *>(1, &r), r, h);
		return i + 1;
	}
	if (r.op == gxlog::OP_DRAW || r.op == gxlog::APPLY_BEGIN) {
		const size_t ab = r.op == gxlog::APPLY_BEGIN ? i : (i + 1 < n && L.rec[i + 1].op == gxlog::APPLY_BEGIN ? i + 1 : n);
		const size_t ae = ab < n ? applyEnd(L, ab, n) : n;
		In in(L, ab < n ? ab + 1 : n, ae, h, p);
		in.collect = collect;
		in.seq = r.entry;
		for (size_t k = in.b; k < ae; ++k)	// provenance inside the Apply (a texture / material Apply's device access):
			if (L.rec[k].op == gxlog::RAW_ACCESS) {
#if GX_L2M_MUTANT != 19
				++s.unknown;
#endif
				h.unknown(u64At(L, L.rec[k]));
			}
		if (r.op == gxlog::OP_DRAW) {
			const DrawRec *dr = L.get<DrawRec>(r.payload);
			DrawArgs a = dr ? dr->args : DrawArgs();
			const DrawOut o = draw(s, in, a, dr ? dr->lowBound : 0, dr ? dr->trianglesOn != 0 : true, dr && dr->hasSort ? &dr->sb : nullptr);
			in.finish();
			compare(L, p.v, nestedIn(L, in.b, ae), r, h);
			h.drawn(r, a, o, s);
		} else {
			apply(s, in);
			in.finish();
			compare(L, p.v, nestedIn(L, in.b, ae), r, h);
		}
		return ab < n ? (ae + 1 < n ? ae + 1 : n) : i + 1;
	}
	// a high-level setter: run its port, then compare with the NESTED invocations logged under it
	switch (r.op) {
	case gxlog::SET_TEXTURE: setTexture(s, r.a, u64At(L, r)); break;
	case gxlog::SET_MATERIAL: setMaterial(s, u64At(L, r)); break;
	case gxlog::SET_SHADER: setShader(s, r.a); break;
	case gxlog::SET_TRANSFORM: if (const Mat4 *m = L.get<Mat4>(r.payload)) setTransform(s, r.a, *m); break;
	case gxlog::SET_WORLD_IDENTITY: setWorldIdentity(s); break;
	case gxlog::SET_VIEW_IDENTITY: setViewIdentity(s); break;
	case gxlog::SET_VB: if (const VbDesc *v = L.get<VbDesc>(r.payload)) setVertexBuffer(s, r.a, *v); break;
	case gxlog::SET_VB_DYNAMIC: if (const VbDesc *v = L.get<VbDesc>(r.payload)) setVertexBufferDynamic(s, *v, (uint16_t)r.a, (uint16_t)r.b); break;
	case gxlog::SET_IB: if (const IbDesc *v = L.get<IbDesc>(r.payload)) setIndexBuffer(s, *v, (uint16_t)r.a); break;
	case gxlog::SET_IB_DYNAMIC: if (const IbDesc *v = L.get<IbDesc>(r.payload)) setIndexBufferDynamic(s, *v, (uint16_t)r.a, (uint16_t)r.b); break;
	case gxlog::SET_INDEX_OFFSET: setIndexOffset(s, (uint16_t)r.a); break;
	case gxlog::SET_LIGHT: setLight(s, r.a, r.b ? L.get<Light>(r.payload) : nullptr); break;
	case gxlog::SET_LIGHT_ENV: setLightEnvironment(s, r.b ? L.get<Lenv>(r.payload) : nullptr); break;
	case gxlog::SET_RENDER_STATE: if (const Logical *l = L.get<Logical>(r.payload)) setRenderState(s, *l); break;
	case gxlog::INVALIDATE: invalidate(s); break;
	case gxlog::ENTRY: h.entry(r.a, r.b, s); break;
	case gxlog::SORTQ:
		if (const Logical *logged = L.get<Logical>(r.payload)) {
			Logical tmp = *logged;	// operator= into the stored copy: only the fields it writes can differ (MH12)
#if GX_L2M_MUTANT == 20
			std::memset(&tmp, 0, sizeof(tmp));	// M20: every field compared, the unwritten disabled-light payloads too
#endif
			copyRenderState(tmp, s.rs);
			h.sortq(r.a, tmp, *logged);
		}
		break;
	case gxlog::RAW_ACCESS:
#if GX_L2M_MUTANT != 19
		++s.unknown;
#endif
		h.unknown(u64At(L, r));
		break;
	case gxlog::APPLY_END: h.callMismatch(r, "APPLY_END without APPLY_BEGIN"); break;
	default: break;	// READ, RESOURCE, CAT_BEGIN, TASK_END, FLUSH_END: markers
	}
	size_t j = i + 1;
	while (j < n && isLow(L.rec[j].op) && (L.rec[j].flags & gxlog::NESTED)) ++j;
	compare(L, p.v, nestedIn(L, i + 1, j), r, h);
	return j;
}

} // namespace

void replay(State &s, const gxlog::Log &L, ReplayHooks &h)
{
	Stepper st(s, L, h);
	const size_t n = L.rec.size();
	for (size_t i = 0; i < n;) i = st.step(i, n);
	h.end(s);
}

// ---- C5 INVARIANCE (PLAN_model T4; CONTRACT_L2 §4.3 C5, §3.4 under DrawRun S1-DR r10) ---------------------------------------------
namespace {

// Arm B's hooks: the boundary replay's invocations have no logged counterpart (MODEL_CALL is C6's, on arm A), so it hears nothing.
struct Quiet : ReplayHooks { void callMismatch(const gxlog::Record &, const char *) override {} };

bool inBracket(const gxlog::Record &r, uint8_t bracket, int stage)
{
	return r.bracket == bracket && (bracket != gxlog::BR_TEXTURE || r.stage == stage) && isLow(r.op);
}

// The input brackets of a boundary-replay Apply (PLAN_model T4): the classic Apply of the SAME object at the corresponding task, or
// the nearest earlier one in the run. No source = c5_unsourced, an INVARIANCE failure (PLAN_model r2 #4), never a skip.
struct SrcIn : Inputs {
	const gxlog::Log &L; const std::vector<Src> &src; uint32_t seq; C5Sink &h5; C5Counts &n;
	SrcIn(const gxlog::Log &l, const std::vector<Src> &s, uint32_t q, C5Sink &h, C5Counts &c) : L(l), src(s), seq(q), h5(h), n(c) {}
	void use(State &s, uint8_t bracket, int stage, uint64_t obj)
	{
		const Src *best = nullptr;
		for (const Src &x : src)	// in classic order: the last one at or before the corresponding task
			if (x.bracket == bracket && x.stage == stage && x.obj == obj && x.seq <= seq) best = &x;
		if (!best) {
			++n.unsourced;
			++n.mismatches;
			char f[32];
			if (bracket == gxlog::BR_MATERIAL) std::snprintf(f, sizeof(f), "unsourced:MATERIAL");
			else std::snprintf(f, sizeof(f), "unsourced:TEX%d", stage);
			h5.invariance(seq, "run", f, obj, 0);
			return;
		}
		for (size_t i = best->b; i < best->e; ++i)
			if (inBracket(L.rec[i], bracket, stage)) replayLow(s, L, L.rec[i]);
	}
	void textureApply(State &s, int stage, uint64_t tex) override { use(s, gxlog::BR_TEXTURE, stage, tex); }
	void materialApply(State &s, uint64_t m) override { use(s, gxlog::BR_MATERIAL, 0, m); }
};

unsigned payloadSize(const gxlog::Record &r)
{
	switch (r.op) {
	case gxlog::DX8_TEXTURE: return 8;
	case gxlog::DX8_MATERIAL: return sizeof(Material);
	case gxlog::DX8_LIGHT: return r.b ? sizeof(Light) : 0;
	case gxlog::DX8_XFORM: case gxlog::XFORM_RAW: return sizeof(Mat4);
	default: return 0;
	}
}

// Two classic Applies of one object: the same logged output (op, arguments, payload bytes - not the sent / skipped tags, which depend
// on the caches they met).
bool sameOutput(const gxlog::Log &L, const Src &x, const Src &y)
{
	size_t i = x.b, j = y.b;
	for (;;) {
		while (i < x.e && !inBracket(L.rec[i], x.bracket, x.stage)) ++i;
		while (j < y.e && !inBracket(L.rec[j], y.bracket, y.stage)) ++j;
		if (i == x.e || j == y.e) return i == x.e && j == y.e;
		const gxlog::Record &p = L.rec[i], &q = L.rec[j];
		if (p.op != q.op || p.a != q.a || p.b != q.b) return false;
		unsigned k = payloadSize(p);
#if GX_L2M_MUTANT == 27
		k = 0;	// M27: the payload bytes ignored
#endif
		const uint8_t *pp = L.get<uint8_t>(p.payload), *qp = L.get<uint8_t>(q.payload);
		if (k && (!pp || !qp || std::memcmp(pp, qp, k) != 0)) return false;
		++i; ++j;
	}
}

struct Cmp {
	C5Sink &h; C5Counts &n; uint32_t seq; const char *pt;
	void bad(const char *f, uint64_t c, uint64_t l) { ++n.mismatches; h.invariance(seq, pt, f, c, l); }
};

// What a draw can see (CONTRACT_L2 §4.3 C5, observationally): a slot valid in arm A must be valid and equal in arm B.
void cmpDevice(const State &x, const State &y, Cmp &c)
{
	const Device &a = x.dev, &b = y.dev;
	char f[32];
	for (int i = 0; i < 256; ++i) {
		if (!a.rsKnown[i]) continue;
		if (!b.rsKnown[i]) { std::snprintf(f, sizeof(f), "RS%d_UNKNOWN", i); c.bad(f, a.rs[i], 0); }
		else if (a.rs[i] != b.rs[i]) { std::snprintf(f, sizeof(f), "RS%d", i); c.bad(f, a.rs[i], b.rs[i]); }
	}
	for (int st = 0; st < kStages; ++st) {
		for (int i = 0; i < 32; ++i) {
			if (!a.tssKnown[st][i]) continue;
			if (!b.tssKnown[st][i]) { std::snprintf(f, sizeof(f), "TSS%d.%d_UNKNOWN", st, i); c.bad(f, a.tss[st][i], 0); }
			else if (a.tss[st][i] != b.tss[st][i]) { std::snprintf(f, sizeof(f), "TSS%d.%d", st, i); c.bad(f, a.tss[st][i], b.tss[st][i]); }
		}
		if (a.tex[st] != b.tex[st]) { std::snprintf(f, sizeof(f), "TEX%d", st); c.bad(f, a.tex[st], b.tex[st]); }
		if ((a.texmValid >> st) & 1u) {
			if (!((b.texmValid >> st) & 1u)) { std::snprintf(f, sizeof(f), "TEXM%d_UNKNOWN", st); c.bad(f, 1, 0); }
			else if (std::memcmp(&a.texm[st], &b.texm[st], sizeof(Mat4))) { std::snprintf(f, sizeof(f), "TEXM%d", st); c.bad(f, 1, 1); }
		}
	}
	if (a.matValid) {
		if (!b.matValid) c.bad("MATERIAL_UNKNOWN", 1, 0);
		else if (std::memcmp(&a.mat, &b.mat, sizeof(Material))) c.bad("MATERIAL", payloadHash(&a.mat, sizeof(Material)), payloadHash(&b.mat, sizeof(Material)));
	}
#if GX_L2M_MUTANT != 29
	// The enable is tracked whatever the payload's validity (Invalidate clears only the validity; Set_DX8_Light's disable tests only the
	// enable): enables always compared; a payload, and its validity, only where the slot is ON - a disabled slot's payload is not
	// observable (a slot enabled and disabled again inside a run leaves arm A a valid payload arm B never had).
	for (int l = 0; l < kLights; ++l) {	// M29: the lights not compared
		if (a.lightOn[l] != b.lightOn[l]) { std::snprintf(f, sizeof(f), "LIGHT%d_ON", l); c.bad(f, a.lightOn[l], b.lightOn[l]); continue; }
#if GX_L2M_MUTANT == 30
		if (!a.lightValid[l]) continue;	// M30: a slot disabled in both still needs arm B's payload valid
		if (!b.lightValid[l]) { std::snprintf(f, sizeof(f), "LIGHT%d_UNKNOWN", l); c.bad(f, 1, 0); continue; }
#endif
		if (!a.lightOn[l] || !a.lightValid[l]) continue;
		if (!b.lightValid[l]) { std::snprintf(f, sizeof(f), "LIGHT%d_UNKNOWN", l); c.bad(f, 1, 0); }
		else if (std::memcmp(&a.light[l], &b.light[l], sizeof(Light))) {
			std::snprintf(f, sizeof(f), "LIGHT%d", l);
			c.bad(f, payloadHash(&a.light[l], sizeof(Light)), payloadHash(&b.light[l], sizeof(Light)));
		}
	}
#endif
	const char *xf[2] = { "WORLD", "VIEW" };
	const Mat4 *am[2] = { &a.world, &a.view }, *bm[2] = { &b.world, &b.view };
	for (int k = 0; k < 2; ++k) {
		if (!((a.xformValid >> k) & 1u)) continue;
		if (!((b.xformValid >> k) & 1u)) { std::snprintf(f, sizeof(f), "%s_UNKNOWN", xf[k]); c.bad(f, 1, 0); }
		else if (std::memcmp(am[k], bm[k], sizeof(Mat4))) c.bad(xf[k], payloadHash(am[k], sizeof(Mat4)), payloadHash(bm[k], sizeof(Mat4)));
	}
	if (a.viewSentValid && (!b.viewSentValid || std::memcmp(&a.viewSent, &b.viewSent, sizeof(Mat4)))) c.bad("VIEWSENT", 1, b.viewSentValid);
	if (a.projValid && (!b.projValid || std::memcmp(&a.proj, &b.proj, sizeof(Mat4)))) c.bad("PROJ", 1, b.projValid);
	if (a.streamKnown) {
		if (!b.streamKnown) c.bad("STREAM_UNKNOWN", a.stream[0], 0);
		else {
			for (int k = 0; k < 2; ++k)
				if (a.stream[k] != b.stream[k] || a.stride[k] != b.stride[k]) { std::snprintf(f, sizeof(f), "STREAM%d", k); c.bad(f, a.stream[k], b.stream[k]); }
			if (a.vertexShader != b.vertexShader) c.bad("FVF", a.vertexShader, b.vertexShader);
		}
	}
	if (a.indicesKnown) {
		if (!b.indicesKnown) c.bad("INDICES_UNKNOWN", a.indices, 0);
		else if (a.indices != b.indices) c.bad("INDICES", a.indices, b.indices);
		else if (a.indexBase != b.indexBase) c.bad("BASEVERTEX", a.indexBase, b.indexBase);
	}
}

// Two RenderStateStruct copies, field by field, reported as rs.<field>.
void cmpRs(const Logical &x, const Logical &y, Cmp &c)
{
	struct Out : FieldSink {
		Cmp &c;
		explicit Out(Cmp &k) : c(k) {}
		void field(const char *name, uint64_t cv, uint64_t lv) override { char f[40]; std::snprintf(f, sizeof(f), "rs.%s", name); c.bad(f, cv, lv); }
	} o(c);
	logicalDiff(x, y, &o);
}

// A unit that can move the DEVICE in the model: not the purely logical setters (their ports write render_state and flags only), so
// the device compare after them would repeat the previous one.
bool movesDevice(uint16_t op)
{
	switch (op) {
	case gxlog::SET_TEXTURE: case gxlog::SET_MATERIAL: case gxlog::SET_SHADER: case gxlog::SET_WORLD_IDENTITY: case gxlog::SET_VIEW_IDENTITY:
	case gxlog::SET_VB: case gxlog::SET_VB_DYNAMIC: case gxlog::SET_IB: case gxlog::SET_IB_DYNAMIC: case gxlog::SET_INDEX_OFFSET:
	case gxlog::SET_LIGHT: case gxlog::SET_RENDER_STATE:
		return false;
	default:
		return op < gxlog::ENTRY;	// markers move nothing
	}
}

// The pending logical state: render_state and its dirty flags.
void cmpLogical(const State &x, const State &y, Cmp &c)
{
	cmpRs(x.rs, y.rs, c);
	if (x.changed != y.changed) c.bad("CHANGED", x.changed, y.changed);
}

// At the flush end, also what the NEXT flush's sends are gated by: the RS / TSS caches and the shader cache.
void cmpCaches(const State &x, const State &y, Cmp &c)
{
	char f[32];
	for (int i = 0; i < 256; ++i)
		if (x.cacheRS[i] != y.cacheRS[i]) { std::snprintf(f, sizeof(f), "CACHE_RS%d", i); c.bad(f, x.cacheRS[i], y.cacheRS[i]); }
	for (int st = 0; st < kStages; ++st)
		for (int i = 0; i < 32; ++i)
			if (x.cacheTSS[st][i] != y.cacheTSS[st][i]) { std::snprintf(f, sizeof(f), "CACHE_TSS%d.%d", st, i); c.bad(f, x.cacheTSS[st][i], y.cacheTSS[st][i]); }
	if (x.curShader != y.curShader) c.bad("SHADER_CACHE", x.curShader, y.curShader);
	if (x.shaderDirty != y.shaderDirty) c.bad("SHADER_DIRTY", x.shaderDirty, y.shaderDirty);
}

} // namespace

void replayC5(State &a, State &b, const gxlog::Log &L, const Run *runs, unsigned nRuns, ReplayHooks &hA, C5Sink &h5, C5Counts &n)
{
	const size_t N = L.rec.size();
	// where each task's records are: its ENTRY .. its TASK_END
	std::unordered_map<uint32_t, size_t> ent, tend;
	for (size_t i = 0; i < N; ++i) {
		const gxlog::Record &r = L.rec[i];
		if (r.op == gxlog::ENTRY && r.b == kEntryTask && !ent.count(r.a)) ent[r.a] = i;
		else if (r.op == gxlog::TASK_END && !tend.count(r.a)) tend[r.a] = i;
	}
	// each run's range: ENTRY(first) .. TASK_END(last), in order, holding only its own TASK entries; else run_range (left as recorded)
	struct Range { size_t b, e; uint32_t first, last; bool scaled; };
	std::vector<Range> rr;
	for (unsigned k = 0; k < nRuns; ++k) {
		const Run &u = runs[k];
		const auto eb = ent.find(u.first), ee = tend.find(u.last);
		bool ok = u.first <= u.last && eb != ent.end() && ee != tend.end() && eb->second < ee->second
			&& (rr.empty() || rr.back().e < eb->second);
		bool scaled = false;
		for (size_t i = ok ? eb->second : 0; ok && i <= ee->second; ++i) {
			const gxlog::Record &r = L.rec[i];
			if (r.op == gxlog::ENTRY) ok = r.b == kEntryTask && r.a >= u.first && r.a <= u.last && tend.count(r.a) && ent.count(r.a);
			if (r.op == gxlog::RS && r.a == RS_NORMALIZENORMALS && r.b != 0 && !(r.flags & gxlog::NESTED)) scaled = true;
		}
		if (!ok) {
			++n.badRanges;
			++n.mismatches;
			h5.invariance(u.first, "run", "run_range", u.first, u.last);
			continue;
		}
		rr.push_back({ eb->second, ee->second, u.first, u.last, scaled });
	}
	Quiet qb;
	Stepper sa(a, L, hA), sb(b, L, qb);
	std::vector<Src> src;
#if GX_L2M_MUTANT == 28
	sa.collect = &src;	// M28: sources not limited to the run (collected everywhere, never cleared)
#endif
	// one task's setters into arm B: the top-level units between its ENTRY and its draw (the NORMALIZENORMALS TRUE only if asked)
	auto setters = [&](uint32_t seq, bool withNN) {
		const size_t stop = tend[seq];
		for (size_t i = ent[seq] + 1; i < stop && L.rec[i].op != gxlog::OP_DRAW;) {
			const gxlog::Record &r = L.rec[i];
			if (!withNN && r.op == gxlog::RS && r.a == RS_NORMALIZENORMALS && !(r.flags & gxlog::NESTED)) { ++i; continue; }
			i = sb.step(i, stop);
		}
	};
	size_t i = 0;
	unsigned k = 0;
	while (i < N) {
		if (k < rr.size() && i == rr[k].b) {
			const Range &g = rr[k++];
			// arm A: the run as recorded (C6 hears it), collecting its input Applies
#if GX_L2M_MUTANT != 28
			src.clear();
			sa.collect = &src;
#endif
			for (size_t j = g.b; j <= g.e;) j = sa.step(j, g.e + 1);
#if GX_L2M_MUTANT != 28
			sa.collect = nullptr;
#endif
			for (size_t x = 0; x < src.size(); ++x)	// payload_stable: re-Applies of one object in the run must log one output
				for (size_t y = 0; y < x; ++y)
					if (src[y].bracket == src[x].bracket && src[y].stage == src[x].stage && src[y].obj == src[x].obj) {
						if (!sameOutput(L, src[y], src[x])) {
							++n.payloadUnstable;
							++n.mismatches;
							h5.invariance(src[x].seq, "run", src[x].bracket == gxlog::BR_MATERIAL ? "payload_unstable:MATERIAL" : "payload_unstable:TEXTURE",
								src[y].seq, src[x].seq);
						}
						break;	// against the object's first Apply in the run only
					}
			// arm B: the boundary replay in its DrawRun form - PREPARE, the wrapper's NORMALIZENORMALS FALSE, CARRY-OUT
			setters(g.first, true);
			{ SrcIn in(L, src, g.first, h5, n); apply(b, in); }
#if GX_L2M_MUTANT != 26
			if (g.scaled) sendRS(b, RS_NORMALIZENORMALS, 0);	// M26: DrawRun's FALSE left out of the wrapper's cache
#endif
			const uint32_t co = b.mutant == 5 ? g.first : g.last;	// M5: the carry-out replays the run's FIRST task
			setters(co, false);
			{ SrcIn in(L, src, co, h5, n); apply(b, in); }
			++n.runs;
			n.scaledRuns += g.scaled;
			i = g.e + 1;
			continue;
		}
		const size_t lim = k < rr.size() ? rr[k].b : N;
		const gxlog::Record &r = L.rec[i];
		if (r.op == gxlog::ENTRY) {	// an unclaimed entry's START: pending logical state and the device
			++n.compares;
			Cmp c{ h5, n, r.a, "entry" };
			cmpLogical(a, b, c);
			cmpDevice(a, b, c);
		} else if (r.op == gxlog::SORTQ) {	// both arms' queued copies (operator=)
			++n.compares;
			Logical ta, tb;
			std::memset(&ta, 0, sizeof(ta));
			std::memset(&tb, 0, sizeof(tb));
			copyRenderState(ta, a.rs);
			copyRenderState(tb, b.rs);
			Cmp c{ h5, n, r.a, "sortq" };
			cmpRs(ta, tb, c);
		}
		const size_t j = sa.step(i, lim);
		sb.step(i, lim);	// the same records: the same unit
		if (movesDevice(r.op)) {	// after every unit that can send: the resulting device state (a cache-gated branch may differ, its result not)
			++n.compares;
			Cmp c{ h5, n, r.entry, r.op == gxlog::OP_DRAW ? "draw" : "call" };
			cmpDevice(a, b, c);
		}
		i = j;
	}
	++n.compares;	// the flush end, after the null-buffer setters (MH20)
	Cmp c{ h5, n, N ? L.rec[N - 1].entry : 0u, "end" };
	cmpDevice(a, b, c);
	cmpLogical(a, b, c);
	cmpCaches(a, b, c);
	hA.end(a);
}

} // namespace gxl2m
