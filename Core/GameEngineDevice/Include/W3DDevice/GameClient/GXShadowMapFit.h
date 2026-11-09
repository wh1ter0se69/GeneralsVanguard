// GeneralsX @feature D · look-shadowmaps 17/10/2026 S7 GPU sun shadow map: the light fit (pure math, no engine include).
//
// PREREG_look-shadowmaps.md A14 + A28 (Codex r2 N8); PLAN Task 5. Host-tested by scripts/test/sm-fit-test.sh.
//
// One orthographic light per cascade, looking along the map's sun direction t (object light 0 of the current time of day, from the
// sun INTO the scene: W3DShadow.cpp:232-251 stores -t as the light position). The receivers are what the camera can see of the terrain
// slab (the map's lowest to highest terrain height): the XY outline of the view pyramid (capped at the far clip) cut by that slab,
// clipped as a polygon to the terrain's world rectangle, and - for the near cascade - to the near part of that BOUNDED outline; then
// stood up as a prism at both terrain heights (Codex code-fit2-r1: clamping vertices one by one shrank the outline and kept far rays'
// altitudes). The cascade's square extent S:
//   * covers the footprint's light-space box plus a guard band of `guard` texels per side (the filter's reach),
//   * is quantized UP to a multiple of `quantum` world units, and changes only by GROWING at once when the need is not
//     contained, or SHRINKING to ceil(need x 1.125 / quantum) x quantum when that is smaller (hysteresis on shrinking only:
//     A28) - so a need hovering at a quantum boundary never flips the extent frame to frame,
//   * is centred on the box with the centre snapped to the texel grid, so a still camera gives bit-identical matrices and a pan
//     moves the map by whole texels (no shimmer).
// Depth: linear along t, from the nearer of the receivers and the casters' own runtime bounds (the walk passes them: airborne
// units, tall structures - never a load-time guess, A28) to the farthest receiver, plus a margin.
// Output matrices are D3D row-vector (_11 _12 ...; v' = v * M) mapping world to Vulkan clip space (z 0..1).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace GXShadowFit {

struct Vec3 { float x, y, z; };
inline Vec3 v3(float x, float y, float z) { Vec3 r = { x, y, z }; return r; }
inline Vec3 add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3 scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline Vec3 normalize(Vec3 a) { const float l = std::sqrt(dot(a, a)); return l > 0.f ? scale(a, 1.f / l) : v3(0.f, 0.f, -1.f); }

struct Basis { Vec3 x, y, f; };   // f = the sun direction (depth axis), x/y span the map plane seen from the sun

// The light basis: right-handed, never degenerate (a sun straight down takes Y as its up).
inline Basis basis(Vec3 sun)
{
	Basis b;
	b.f = normalize(sun);
	const Vec3 up = std::fabs(b.f.z) > 0.99f ? v3(0.f, 1.f, 0.f) : v3(0.f, 0.f, 1.f);
	b.x = normalize(cross(up, b.f));
	b.y = cross(b.f, b.x);
	return b;
}

struct Footprint { Vec3 p[64]; int n = 0; };

// The view pyramid: out[0] = the eye, out[1 + i] = eye + dir[i] x far. dir[i] are the corner rays scaled so their CAMERA-SPACE depth is 1
// (W3D: (x, y, -1) on the view plane, rotated into the world - not normalized), so `far` is the camera's own far clip depth (OBSERVED
// 17/10 on the Mini: with a far of 20000 wu along unit rays, an observer camera's top rays never reached the ground and cascade 0 came
// out 10,752 wu wide). dir[0..1] = the BOTTOM corners (the near edge), dir[2..3] = the TOP corners.
inline void frustumCorners(Vec3 eye, const Vec3 dir[4], float farDist, Vec3 out[5])
{
	out[0] = eye;
	for (int i = 0; i < 4; ++i)
		out[1 + i] = add(eye, scale(dir[i], farDist));
}

struct Poly { float x[64], y[64]; int n = 0; };

// keep the part of a convex polygon with ax * x + ay * y <= c (Sutherland-Hodgman, one plane: the edge crossings are kept)
inline Poly clipHalf(const Poly& in, float ax, float ay, float c)
{
	Poly o;
	for (int i = 0; i < in.n && o.n < 62; ++i) {
		const int j = i + 1 == in.n ? 0 : i + 1;
		const float di = ax * in.x[i] + ay * in.y[i] - c, dj = ax * in.x[j] + ay * in.y[j] - c;
		if (di <= 0.f) {
			o.x[o.n] = in.x[i]; o.y[o.n] = in.y[i]; ++o.n;
		}
		if ((di < 0.f && dj > 0.f) || (di > 0.f && dj < 0.f)) {
			const float t = di / (di - dj);
			o.x[o.n] = in.x[i] + t * (in.x[j] - in.x[i]); o.y[o.n] = in.y[i] + t * (in.y[j] - in.y[i]); ++o.n;
		}
	}
	return o;
}

// the convex hull of n XY points (Andrew's monotone chain; n <= 32), counter-clockwise
inline Poly hullXY(float* px, float* py, int n)
{
	for (int i = 1; i < n; ++i)   // insertion sort by (x, y): n is tiny
		for (int j = i; j > 0 && (px[j] < px[j - 1] || (px[j] == px[j - 1] && py[j] < py[j - 1])); --j) {
			const float tx = px[j], ty = py[j];
			px[j] = px[j - 1]; py[j] = py[j - 1]; px[j - 1] = tx; py[j - 1] = ty;
		}
	Poly h;
	if (n < 3) {
		for (int i = 0; i < n; ++i) { h.x[i] = px[i]; h.y[i] = py[i]; }
		h.n = n;
		return h;
	}
	auto turn = [&](int a, int b, int c) {
		return (h.x[b] - h.x[a]) * (py[c] - h.y[a]) - (h.y[b] - h.y[a]) * (px[c] - h.x[a]);
	};
	int k = 0;
	for (int i = 0; i < n; ++i) {
		while (k >= 2 && turn(k - 2, k - 1, i) <= 0.f) --k;
		h.x[k] = px[i]; h.y[k] = py[i]; ++k;
	}
	for (int i = n - 2, lo = k + 1; i >= 0; --i) {
		while (k >= lo && turn(k - 2, k - 1, i) <= 0.f) --k;
		h.x[k] = px[i]; h.y[k] = py[i]; ++k;
	}
	h.n = k - 1;   // the last point repeats the first
	return h;
}

// The receivers of one cascade. The view pyramid (eye + the four corner rays to `far`, frustumCorners) is cut by the slab
// zLo <= z <= zHi: its vertices inside the slab plus every pyramid edge's crossings of both planes give the exact XY outline of what
// the camera sees of the terrain's height range (the near clip is ignored: conservative). That outline is clipped AS A POLYGON to
// `clampXY` (xmin, ymin, xmax, ymax; null = none: there is no receiver past the terrain, Codex code-fit2-r1 #1), and for the near
// cascade (`cut` < 1) to its near part: dot(p, u) <= dmin + cut x (dmax - dmin) along u = the top-mid ray's ground direction, measured
// on the BOUNDED outline (#4). Its vertices are then emitted at both heights zLo and zHi, so the depth range is the receivers' own
// (#2). n = 0: the camera sees no terrain (the fit still returns a finite matrix).
inline Footprint footprint(Vec3 eye, const Vec3 dir[4], float zLo, float zHi, float farDist, float cut, const float* clampXY = nullptr)
{
	Footprint fp;
	Vec3 v[5];
	frustumCorners(eye, dir, farDist, v);
	float px[32], py[32];
	int n = 0;
	for (int i = 0; i < 5; ++i)
		if (v[i].z >= zLo && v[i].z <= zHi) { px[n] = v[i].x; py[n] = v[i].y; ++n; }
	// the 8 pyramid edges: eye to each far corner, and the far rectangle 0-1-3-2 (bottom-left, bottom-right, top-right, top-left)
	static const int kEdge[8][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 2, 4 }, { 4, 3 }, { 3, 1 } };
	const float planes[2] = { zLo, zHi };
	for (int e = 0; e < 8; ++e)
		for (int k = 0; k < 2; ++k) {
			const Vec3 a = v[kEdge[e][0]], b = v[kEdge[e][1]];
			const float da = a.z - planes[k], db = b.z - planes[k];
			if ((da < 0.f && db > 0.f) || (da > 0.f && db < 0.f)) {
				const float t = da / (da - db);
				px[n] = a.x + t * (b.x - a.x); py[n] = a.y + t * (b.y - a.y); ++n;
			}
		}
	Poly poly = hullXY(px, py, n);
	if (clampXY) {
		poly = clipHalf(poly, -1.f, 0.f, -clampXY[0]);
		poly = clipHalf(poly, 1.f, 0.f, clampXY[2]);
		poly = clipHalf(poly, 0.f, -1.f, -clampXY[1]);
		poly = clipHalf(poly, 0.f, 1.f, clampXY[3]);
	}
	if (cut < 1.f && poly.n > 0) {
		float ux = dir[2].x + dir[3].x, uy = dir[2].y + dir[3].y;
		const float ul = std::sqrt(ux * ux + uy * uy);
		if (ul > 1e-6f) {
			ux /= ul; uy /= ul;
			float d0 = 1e30f, d1 = -1e30f;
			for (int i = 0; i < poly.n; ++i) {
				const float d = ux * poly.x[i] + uy * poly.y[i];
				d0 = d < d0 ? d : d0; d1 = d > d1 ? d : d1;
			}
			poly = clipHalf(poly, ux, uy, d0 + cut * (d1 - d0));
		}
	}
	for (int i = 0; i < poly.n && fp.n + 2 <= 64; ++i) {
		fp.p[fp.n++] = v3(poly.x[i], poly.y[i], zLo);
		fp.p[fp.n++] = v3(poly.x[i], poly.y[i], zHi);
	}
	return fp;
}

struct State { float extent = 0.f; };   // the cascade's current extent (0 = none yet): kept by the caller across frames

struct Params
{
	uint32_t res = 2048;      // texels per side of the cascade rect
	uint32_t guard = 5;       // texels of guard band per side (the step's max filter radius + 1)
	float quantum = 256.f;    // world units
	float shrinkBand = 1.125f;
	float depthMargin = 16.f; // world units on both depth ends
};

struct Result
{
	float m[16];              // world -> clip, row-vector, D3D element order
	float extent, texel, cx, cy, zNear, zFar;
};

// casterNear / casterValid: the nearest (smallest) light-space depth of the walk's casters this frame.
inline Result fit(const Basis& b, const Footprint& fp, float casterNear, bool casterValid, const Params& pr, State* st)
{
	float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
	for (int i = 0; i < fp.n; ++i) {
		const float lx = dot(fp.p[i], b.x), ly = dot(fp.p[i], b.y), lz = dot(fp.p[i], b.f);
		x0 = lx < x0 ? lx : x0; x1 = lx > x1 ? lx : x1;
		y0 = ly < y0 ? ly : y0; y1 = ly > y1 ? ly : y1;
		z0 = lz < z0 ? lz : z0; z1 = lz > z1 ? lz : z1;
	}
	if (fp.n == 0) {   // no receivers (the camera sees no terrain): any finite fit will do, nothing samples it
		x0 = x1 = y0 = y1 = z0 = z1 = 0.f;
	}
	if (casterValid && casterNear < z0)
		z0 = casterNear;
	const float span = (x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0);
	// the guard texels come out of the rect: need = span / (1 - 2 guard / res), +1 texel for the centre snap; never below one quantum
	// (an empty or degenerate outline must not give a zero extent: Codex code-fit2-r1 #1)
	const float usable = 1.f - 2.f * float(pr.guard + 1) / float(pr.res);
	const float needRaw = span / (usable > 0.1f ? usable : 0.1f);
	const float need = needRaw > pr.quantum ? needRaw : pr.quantum;
	const float q = std::ceil(need / pr.quantum) * pr.quantum;
	// shrink to the smallest quantized size that still keeps the band of headroom (Codex code-t4-r1 P2: comparing the band against
	// q itself could never shrink a large extent after a zoom-in); a need hovering at a quantum boundary never flips it
	const float shrinkTo = std::ceil(need * pr.shrinkBand / pr.quantum) * pr.quantum;
	float S = st->extent;
#ifndef GX_SM_FIT_MUTANT_NOGROW            // TEST-ONLY mutant (scripts/test/sm-fit-test.sh X- build): the growth rule removed
	if (S < need)                       // A28: grow at once - never clip a receiver
		S = q;
	else
#endif
	if (S == 0.f || shrinkTo < S)
		S = S == 0.f ? q : shrinkTo;
	st->extent = S;
	Result r;
	r.extent = S;
	r.texel = S / float(pr.res);
	r.cx = std::floor((0.5f * (x0 + x1)) / r.texel + 0.5f) * r.texel;
	r.cy = std::floor((0.5f * (y0 + y1)) / r.texel + 0.5f) * r.texel;
	r.zNear = z0 - pr.depthMargin;
	r.zFar = z1 + pr.depthMargin;
	const float sx = 2.f / S, sz = 1.f / (r.zFar - r.zNear);
	// out_j = sum_i p_i M[i][j] + M[3][j]
	r.m[0] = b.x.x * sx; r.m[1] = b.y.x * sx; r.m[2] = b.f.x * sz; r.m[3] = 0.f;
	r.m[4] = b.x.y * sx; r.m[5] = b.y.y * sx; r.m[6] = b.f.y * sz; r.m[7] = 0.f;
	r.m[8] = b.x.z * sx; r.m[9] = b.y.z * sx; r.m[10] = b.f.z * sz; r.m[11] = 0.f;
	r.m[12] = -r.cx * sx; r.m[13] = -r.cy * sx; r.m[14] = -r.zNear * sz; r.m[15] = 1.f;
	return r;
}

// world point -> clip (for tests and for the receivers' check): row-vector product with the fit's matrix
inline Vec3 toClip(const float m[16], Vec3 p)
{
	return v3(p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
		p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]);
}


// ---- PREREG A39: the PERSPECTIVE map from 2003's point light ---------------------------------------------------------------------
// 2003 extrudes every volume from ONE point, L = getLightPosWorld(0) (normalize(-lightPos) x 10000 from the world origin), per vertex
// (W3DVolumetricShadow.cpp constructVolume: ev - L in object space). A depth map seen from L therefore answers 2003's own question for
// every unclamped caster - "is caster geometry between L and this receiver" - on every receiver. The image plane is fixed per (map, L):
// its axis is L -> the terrain rectangle's centre (pointBasis), so cascades are off-axis windows on ONE texel grid: a still camera gives
// bit-identical matrices and a pan moves the map by whole texels. Extents are in TANGENT units (image plane at distance 1).

struct Vec4 { float x, y, z, w; };
inline Vec4 toClip4(const float m[16], Vec3 p)
{
	Vec4 r = { p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
		p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14], p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15] };
	return r;
}
// NDC (the receiver's p.xyz / p.w); w <= 0 (at or behind the light) gives a point far outside every window
inline Vec3 toNdc(const float m[16], Vec3 p)
{
	const Vec4 c = toClip4(m, p);
	return c.w > 1e-6f ? v3(c.x / c.w, c.y / c.w, c.z / c.w) : v3(1e30f, 1e30f, -1.f);
}

struct PointBasis { Vec3 L; Basis b; float dCentre = 1.f; };
inline PointBasis pointBasis(Vec3 L, Vec3 centre)
{
	PointBasis pb;
	pb.L = L;
	const Vec3 a = add(centre, scale(L, -1.f));
	pb.dCentre = std::sqrt(dot(a, a));
	pb.b = basis(a);
	return pb;
}
// distance along the axis from L (the perspective depth before the projection)
inline float axisDist(const PointBasis& pb, Vec3 p) { return dot(add(p, scale(pb.L, -1.f)), pb.b.f); }

// A39 guard: the half-angle (degrees) of the cone around the axis that holds the terrain rectangle at both heights. A single
// perspective map needs it well under 90; the caller runs today's volumes for a (map, L) over kConeMaxDeg (no shipped map: 37.2 max).
const float kConeMaxDeg = 60.f;
inline float coneHalfAngleDeg(const PointBasis& pb, const float rect[4], float zLo, float zHi)
{
	float worst = 0.f;
	for (int k = 0; k < 8; ++k) {
		const Vec3 q = add(v3(rect[(k & 1) ? 2 : 0], rect[(k & 2) ? 3 : 1], (k & 4) ? zHi : zLo), scale(pb.L, -1.f));
		const float c = dot(normalize(q), pb.b.f);
		const float deg = std::acos(c > 1.f ? 1.f : (c < -1.f ? -1.f : c)) * 57.29577951f;
		worst = deg > worst ? deg : worst;
	}
	return worst;
}

struct PointResult
{
	float m[16];              // world -> clip (with w), row-vector, D3D element order; NDC = clip / w, z 0..1 (Vulkan)
	float extent = 0.f, tanTexel = 0.f, cu = 0.f, cv = 0.f, zNear = 0.f, zFar = 0.f;   // tangent units / axis distances (wu)
	bool ok = false;          // false: a receiver at or behind L's plane - the caller runs volumes this frame
};

// One cascade's perspective fit. The prism vertices (footprint) project through L; the window is their box plus the guard band, in
// tangent units, with the ortho fit's quantum (pr.quantum wu at the axis distance dCentre) and A28 hysteresis; the centre is snapped to
// the tangent texel grid. Depth: near = the nearer of the walk's casters (their axis distance minus radius) and the receivers, far = the
// farthest receiver, each with pr.depthMargin; standard perspective z (0 at near, 1 at far). The atlas keeps depthClamp: a caster nearer
// than `near` still writes 0.
inline PointResult fitPoint(const PointBasis& pb, const Footprint& fp, float casterNear, bool casterValid, const Params& pr, State* st)
{
	PointResult r;
	float u0 = 1e30f, u1 = -1e30f, w0 = 1e30f, w1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
	const float minD = 1.f;
	for (int i = 0; i < fp.n; ++i) {
		const Vec3 q = add(fp.p[i], scale(pb.L, -1.f));
		const float d = dot(q, pb.b.f);
		if (!(d > minD))
			return r;   // ok = false
		const float u = dot(q, pb.b.x) / d, w = dot(q, pb.b.y) / d;
		u0 = u < u0 ? u : u0; u1 = u > u1 ? u : u1;
		w0 = w < w0 ? w : w0; w1 = w > w1 ? w : w1;
		d0 = d < d0 ? d : d0; d1 = d > d1 ? d : d1;
	}
	if (fp.n == 0) {   // no receivers: a finite fit around the axis (nothing samples it)
		u0 = u1 = w0 = w1 = 0.f;
		d0 = d1 = pb.dCentre;
	}
	if (casterValid && casterNear < d0)
		d0 = casterNear;
	const float span = (u1 - u0) > (w1 - w0) ? (u1 - u0) : (w1 - w0);
	const float quantum = pr.quantum / (pb.dCentre > 1.f ? pb.dCentre : 1.f);
	const float usable = 1.f - 2.f * float(pr.guard + 1) / float(pr.res);
	const float needRaw = span / (usable > 0.1f ? usable : 0.1f);
	const float need = needRaw > quantum ? needRaw : quantum;
	const float q = std::ceil(need / quantum) * quantum;
	const float shrinkTo = std::ceil(need * pr.shrinkBand / quantum) * quantum;
	float S = st->extent;
#ifndef GX_SM_FIT_MUTANT_NOGROW
	if (S < need)
		S = q;
	else
#endif
	if (S == 0.f || shrinkTo < S)
		S = S == 0.f ? q : shrinkTo;
	st->extent = S;
	r.extent = S;
	r.tanTexel = S / float(pr.res);
	r.cu = std::floor((0.5f * (u0 + u1)) / r.tanTexel + 0.5f) * r.tanTexel;
	r.cv = std::floor((0.5f * (w0 + w1)) / r.tanTexel + 0.5f) * r.tanTexel;
	r.zNear = d0 - pr.depthMargin;
	r.zNear = r.zNear > minD ? r.zNear : minD;
	r.zFar = d1 + pr.depthMargin;
	if (!(r.zFar > r.zNear + 1.f))
		r.zFar = r.zNear + 1.f;
	// clip.x = (2/S)(dot(q, x) - cu d), clip.y = (2/S)(dot(q, y) - cv d), clip.w = d, clip.z = A d + B (z/w = 0 at near, 1 at far),
	// q = p - L, d = dot(q, f): every row is linear in p, the constant column is -dot(L, a) (+ B)
	const float k = 2.f / S, A = r.zFar / (r.zFar - r.zNear), B = -r.zNear * r.zFar / (r.zFar - r.zNear);
	const Vec3 f = pb.b.f;
	const Vec3 aX = scale(add(pb.b.x, scale(f, -r.cu)), k), aY = scale(add(pb.b.y, scale(f, -r.cv)), k), aZ = scale(f, A);
	r.m[0] = aX.x; r.m[1] = aY.x; r.m[2] = aZ.x; r.m[3] = f.x;
	r.m[4] = aX.y; r.m[5] = aY.y; r.m[6] = aZ.y; r.m[7] = f.y;
	r.m[8] = aX.z; r.m[9] = aY.z; r.m[10] = aZ.z; r.m[11] = f.z;
	r.m[12] = -dot(pb.L, aX); r.m[13] = -dot(pb.L, aY); r.m[14] = -dot(pb.L, aZ) + B; r.m[15] = -dot(pb.L, f);
	r.ok = true;
	return r;
}

// the axis distance a perspective NDC depth stands for (the receivers' contact hardening linearises the same way)
inline float ndcToDist(float z, float n, float f) { return n * f / (f - z * (f - n)); }

// ---- A39/A41: a clamped instance's own light (a VERTICAL SCALE about its ground plane) ------------------------------------------
// 2003 raises the light of an instance whose elevation clamp binds STRAIGHT UP: L_c = (L.x, L.y, |L.xy| x lengthScale)
// (W3DVolumetricShadow.cpp updateMeshVolume, the m_shadowLengthScale block). The affine map z' = z0 + mu (z - z0), mu = (L.z - z0) /
// (L_c.z - z0), keeps x/y, fixes the plane z = z0 and sends L_c to L - so it sends every line through L_c to a line through L. Applied to
// the instance's caster copy AND to its own receivers' lookups, the map seen from L shows exactly what 2003's volumes from L_c show for that
// instance: its ground shadow on z0 and its self-shadowing (PREREG A41 P1; Codex a39-design-r1 and sm-fit-test A39-6/7 measured the
// linearised shear it replaces). mu depends on z0 and the clamp only.
// packed: low 32 bits = mu's float bits, high 32 = z0's; 0 = none. The same 64 bits reach the caster capture (World x S) and the
// receiver's pad words (OwnScale, OwnZ0).
struct OwnLight { float mu = 1.f, z0 = 0.f; uint64_t packed = 0; };

// bound = false when the clamp does not bind (2003 keeps L: the map's own ray is 2003's), or when the configuration is not a valid
// light above the plane (the caller's validity guard then decides: A41 P2-7)
inline OwnLight ownLight(Vec3 L, float lengthScale, float z0, bool* bound)
{
	OwnLight o;
	*bound = false;
	if (!(lengthScale > 0.f) || !(L.z > z0 + 1.f))
		return o;
	const float lz = std::sqrt(L.x * L.x + L.y * L.y) * lengthScale;
	if (!(lz > L.z) || !std::isfinite(lz))
		return o;
	const float mu = (L.z - z0) / (lz - z0);
	if (!(mu > 0.f && mu < 1.f) || !std::isfinite(mu))
		return o;
	o.mu = mu;
	o.z0 = z0;
	uint32_t mb, zb;
	std::memcpy(&mb, &mu, 4);
	std::memcpy(&zb, &z0, 4);
	o.packed = (uint64_t(zb) << 32) | uint64_t(mb);
	*bound = true;
	return o;
}
inline OwnLight unpackOwnLight(uint64_t packed)
{
	OwnLight o;
	if (packed == 0)
		return o;
	const uint32_t mb = uint32_t(packed), zb = uint32_t(packed >> 32);
	std::memcpy(&o.mu, &mb, 4);
	std::memcpy(&o.z0, &zb, 4);
	o.packed = packed;
	return o;
}
// A79 / A79a / A79b (E · look-shadowmaps-7): the OWN-LIGHT PULL's game-side half. The device replays a thin copy pulled toward L
// (GXRender Record/GxOwnPull.h: P(p) = L + (1 - eps)(p - L)) and its receivers look up pulled the same way; the class bound and eps
// travel in the frame (GxShadowFrame::ownPull / ownPullAux), so THIS is the one product source of muMax.
static constexpr float kOwnPullMuMax = 0.03f;   // A79b: "thin copies" - flattened to <= 3 % of their height, whatever the clamp's origin
// the class test (the device's gxOwnPullApplies is the same expression; the host test compares the two over a grid)
inline bool ownPullApplies(float mu, float muMax, float eps)
{
	return mu > 0.f && mu <= muMax && eps > 0.f;
}
// A79a: eps = delta / D, D = |L - g|, g = the camera's centre ray hit on z = zMid (dCentre when the ray does not reach it: looking up or
// level), so the pull is ~delta wu at the view (D is 6,000-14,000 wu: a ~1e-3 eps). delta <= 0 or a degenerate D: 0 (no pull).
inline float ownPullEps(Vec3 L, Vec3 eye, Vec3 dir, float zMid, float dCentre, float delta, float* D)
{
	float dist = dCentre;
	if (dir.z < -1e-3f)
	{
		const float t = (zMid - eye.z) / dir.z;
		if (t > 0.f && std::isfinite(t))
		{
			const Vec3 g = add(eye, scale(dir, t));
			const Vec3 q = add(g, scale(L, -1.f));
			dist = std::sqrt(dot(q, q));
		}
	}
	if (D)
		*D = dist;
	return (delta > 0.f && std::isfinite(dist) && dist > 4.f * delta) ? delta / dist : 0.f;
}
// A79b #8: a pulled caster sphere's near bound. near = (its centre's axis distance from L) - radius, as the walk computes it; P scales the
// centre's axis distance by 1 - eps and the radius is kept (P contracts, so the unscaled radius still bounds the copy)
inline float pulledNear(float nearDist, float radius, float eps)
{
#ifdef GX_SM_PULL_MUTANT_NONEAR   // TEST-ONLY mutant (scripts/test/sm-fit X- build): the near bound not pulled (A79-5 must FAIL)
	(void)radius; (void)eps;
	return nearDist;
#else
	return (1.f - eps) * (nearDist + radius) - radius;
#endif
}

// the scale as a D3D row-vector matrix (v' = v * S)
inline void scaleMatrix(const OwnLight& o, float S[16])
{
	const float m[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, o.mu, 0.f, 0.f, 0.f, o.z0 * (1.f - o.mu), 1.f };
	std::memcpy(S, m, sizeof(m));
}
inline Vec3 applyOwn(const OwnLight& o, Vec3 p) { return v3(p.x, p.y, o.z0 + o.mu * (p.z - o.z0)); }

// ---- PREREG A44 (D · look-shadowmaps-4 18/10/2026): the OFF-SCREEN casters' selection --------------------------------------------
// A caster can shadow a visible receiver only if it lies between L and that receiver - inside the light's clip volume through the
// receivers' window. sphereOutsideClip tests a bounding sphere against THE matrix the device renders the cascade with (row-vector,
// clip_j = sum_i p_i m[i*4+j] + m[12+j]; inside = |x| <= w, |y| <= w, z <= w), so one test serves the perspective fit and the TEST-ONLY
// orthographic one. Planes w -+ x, w -+ y and the far plane w - z, each normalised; outside = wholly beyond one of them. The near plane is
// NOT a cull: the atlas keeps depthClamp (a caster nearer than near still writes depth 0). Conservative near a corner (kept, never lost).
inline bool sphereOutsideClip(const float m[16], Vec3 c, float r)
{
	static const int kPlane[5][2] = { { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { 2, 1 } };   // { clip component, sign }: w - sign x_j
	for (int k = 0; k < 5; ++k) {
		const int j = kPlane[k][0];
		const float sg = float(kPlane[k][1]);
		const Vec3 n = v3(m[3] - sg * m[j], m[7] - sg * m[4 + j], m[11] - sg * m[8 + j]);
		const float d = m[15] - sg * m[12 + j];
		const float len = std::sqrt(dot(n, n));
		if (len > 0.f && dot(n, c) + d < -r * len)
			return true;
	}
	return false;
}
// a clamped root's sphere goes through its own light's vertical scale, like its capture (the radius still bounds: the scale only
// shrinks vertically); no own light = unchanged
inline Vec3 ownSphereCentre(const OwnLight& o, Vec3 c)
{
#ifdef GX_SM_OFFSCREEN_MUTANT_UNSCALED   // TEST-ONLY mutant (sm-fit-test.sh): the unscaled centre - A44-2 must FAIL
	(void)o;
	return c;
#else
	return o.packed ? applyOwn(o, c) : c;
#endif
}
// per walk root: outside the light volume -> nothing; wholly INSIDE the camera -> the scene draws it with CAST (as today); else the early
// cast-only flush submits it (off screen, or straddling the screen edge: its in-view meshes are then captured twice - the same depth)
enum OffscreenPick { kOutside, kSkipInView, kSubmit };
inline OffscreenPick pickRoot(bool lightOutside, bool mainInside)
{
	return lightOutside ? kOutside : (mainInside ? kSkipInView : kSubmit);
}
// A47 (Codex a44-code-r1 #1/#2): the decision is per caster MESH of a candidate root. Skipped (the scene captures it with CAST) only when
// the scene will draw its ROOT (RTS3DScene::Visibility_Check replicated by the glue) AND the mesh's sphere grown by insideMargin is inside
// the camera - the margin covers the scene's own re-pose of a single animation (one frame newer than the lazy pose read here)
inline float insideMargin(float r) { return r * 0.1f > 8.f ? r * 0.1f : 8.f; }
// A48 (Codex a47-code-r1 #1): the LOD an additional HLOD is drawn at - its parent's (clamped to its own range, as Set_LOD_Level clamps)
// when the parent matches sub-object LODs (HLodClass::Render), else its own stored LOD. Nothing calls Set_LOD_Level: this only reads.
inline int effectiveLod(int parentLod, bool parentMatches, int ownLod, int lodCount)
{
#ifdef GX_SM_OFFSCREEN_MUTANT_STOREDLOD   // TEST-ONLY mutant (sm-fit-test.sh): the stored LOD always - A44-5 must FAIL
	(void)parentLod; (void)parentMatches; (void)lodCount;
	return ownLod;
#else
	if (!parentMatches)
		return ownLod;
	const int hi = lodCount > 0 ? lodCount - 1 : 0;
	return parentLod < 0 ? 0 : (parentLod > hi ? hi : parentLod);
#endif
}
// A61/A63 (D · look-shadowmaps-5; Codex a61-design-r1 P2-2): a SORTED caster mesh (SORT flag: dx8renderer.cpp:2130 sends it to the
// sorting renderer, past the CAST hook) is never captured by the scene's own draw, so the in-view skip does not apply to it: submitted
// iff it is not outside the light volume, on or off screen. The glue calls only this predicate.
inline OffscreenPick meshPick(bool lightOutside, bool sceneDrawsRoot, bool grownInside, bool sorted = false)
{
#ifndef GX_SM_PICK_MUTANT_SORTED_SKIP   // TEST-ONLY mutant (sm-fit-test.sh): the sorted flag ignored - A61-pick must FAIL
	if (sorted)
		return lightOutside ? kOutside : kSubmit;
#else
	(void)sorted;
#endif
#ifdef GX_SM_OFFSCREEN_MUTANT_ROOTGATE   // TEST-ONLY mutant (sm-fit-test.sh): the root gate dropped - A44-4 must FAIL
	(void)sceneDrawsRoot;
	return pickRoot(lightOutside, grownInside);
#else
	return pickRoot(lightOutside, sceneDrawsRoot && grownInside);
#endif
}

} // namespace GXShadowFit
