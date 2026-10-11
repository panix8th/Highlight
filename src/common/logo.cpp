#include "logo.h"

#include <cmath>

namespace hl {

namespace {

struct Corner {
	float x, y, r; /* corner point and the radius it is rounded with */
};

/* Replaces each corner with a circular arc tangent to both edges (convex and
 * concave corners alike). */
std::vector<LogoPoint> rounded(const std::vector<Corner> &poly, int segments)
{
	const float pi = 3.14159265f;
	std::vector<LogoPoint> out;
	const size_t n = poly.size();
	for (size_t i = 0; i < n; i++) {
		const Corner &a = poly[(i + n - 1) % n], &p = poly[i], &b = poly[(i + 1) % n];
		if (p.r <= 0) {
			out.push_back({p.x, p.y});
			continue;
		}
		float u1x = a.x - p.x, u1y = a.y - p.y, u2x = b.x - p.x, u2y = b.y - p.y;
		const float l1 = std::sqrt(u1x * u1x + u1y * u1y), l2 = std::sqrt(u2x * u2x + u2y * u2y);
		u1x /= l1, u1y /= l1, u2x /= l2, u2y /= l2;
		const float angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, u1x * u2x + u1y * u2y)));
		const float d = p.r / std::tan(angle / 2); /* corner to tangent point */
		float bx = u1x + u2x, by = u1y + u2y;
		const float bl = std::sqrt(bx * bx + by * by);
		bx /= bl, by /= bl;
		const float cd = p.r / std::sin(angle / 2); /* corner to arc centre */
		const float cx = p.x + bx * cd, cy = p.y + by * cd;
		const float a1 = std::atan2(p.y + u1y * d - cy, p.x + u1x * d - cx);
		const float a2 = std::atan2(p.y + u2y * d - cy, p.x + u2x * d - cx);
		float da = a2 - a1;
		while (da > pi)
			da -= 2 * pi;
		while (da < -pi)
			da += 2 * pi;
		for (int k = 0; k <= segments; k++) {
			const float t = a1 + da * (float)k / (float)segments;
			out.push_back({cx + p.r * std::cos(t), cy + p.r * std::sin(t)});
		}
	}
	return out;
}

std::vector<std::vector<LogoPoint>> build()
{
	const float bar = 2.2f, tri = 2.2f, notch = 1.75f, notch_edge = 0.8f;
	const int seg = 6;
	return {
		/* left bar: both bars are slanted 18 units over their 29 unit width */
		rounded({{0, 18, bar}, {29, 0, bar}, {29, 92, bar}, {0, 110, bar}}, seg),
		/* right bar, with the notch the triangle points into: its edges run parallel to
		 * the triangle's (an even gap) and it ends in a short flat tip */
		rounded({{67, 18, bar},
			 {96, 0, bar},
			 {96, 92, bar},
			 {67, 110, bar},
			 {67, 70.6f, notch_edge},
			 {84.6f, 60, notch},
			 {84.6f, 50, notch},
			 {67, 39.4f, notch_edge}},
			seg),
		/* play triangle, centred on the bars */
		rounded({{38, 32.75f, tri}, {73.5f, 55, tri}, {38, 77.25f, tri}}, seg),
	};
}

} // namespace

const std::vector<std::vector<LogoPoint>> &logo_mark()
{
	static const std::vector<std::vector<LogoPoint>> mark = build();
	return mark;
}

LogoPlacement logo_icon_placement(float tile)
{
	LogoPlacement p;
	float s = tile * logo_mark_fraction(tile) / k_logo_h;
	if (tile <= 48.0f) {
		/* whole-pixel bars when that changes the size only a little */
		const float bar = std::round(29.0f * s);
		if (bar >= 2.0f && std::fabs(bar - 29.0f * s) <= 0.3f)
			s = bar / 29.0f;
		p.x = std::round((tile - k_logo_w * s) / 2);
		p.y = std::round((tile - k_logo_h * s) / 2);
		/* the right bar's and the triangle's vertical edges onto the pixel grid too */
		p.dx[1] = std::round(p.x + 67.0f * s) - (p.x + 67.0f * s);
		p.dx[2] = std::round(p.x + 38.0f * s) - (p.x + 38.0f * s);
	} else {
		p.x = (tile - k_logo_w * s) / 2;
		p.y = (tile - k_logo_h * s) / 2;
	}
	p.scale = s;
	return p;
}

} // namespace hl
