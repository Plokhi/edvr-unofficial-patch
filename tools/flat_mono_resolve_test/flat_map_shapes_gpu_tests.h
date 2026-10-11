// The map's test content through the 960x540 -> 1920x1080 path (2026-10-10, the maintainer's request): axis-aligned lines (1 and 2 input
// pixels wide, at integer and half-pixel offsets), a filled circle, a thin tilted ellipse ring, and the slope and diagonal edges as secondary
// cases. Each is run through two variants: EASU alone (SMAA forced off) and SMAA then EASU, the shipped order (2026-10-10).
//
// Shapes are defined in OUTPUT coordinates: pixel X covers [X, X + 1). The input is the shape sampled at the centre of each 2 x 2 block (what a
// 960x540 render of it gives). Measures, all in display values (the 8-bit output's grey level):
//  - lines: the cross-section through the middle of the line: energy ratio (no blur, no doubling: 1 is the ideal), centroid shift (output px)
//    and peak ratio, against the analytic cross-section;
//  - curves: the 0.5-level crossing's deviation from the true curve (RMS and max, in output px), by ray for the circle and ring (720 rays,
//    outer and inner edges) or by scan for the slopes, with the RMS of the row-to-row step error (the visible staircase);
//  - coverage error: mean absolute error over the partly covered output pixels (the earlier metric, kept).
// Validation first: the nearest-neighbour upscale of the binary input (jagged, must score clearly) and the analytic coverage image (must score
// about zero) go through the same measures.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <cstdio>
#include <vector>

namespace shapesgpu {

struct Shape {
    enum Kind { VLine, HLine, Circle, Ring, VSlope, HSlope };
    Kind kind = VLine;
    const char* name = "";
    double a = 0, b = 0;   // lines: the range [a, b); circle: radius a; ring: outer semi-axes a, b
    double cx = 0, cy = 0; // circle and ring centre
    double ai = 0, bi = 0; // ring: inner semi-axes
    double phi = 0;        // ring: rotation (radians)
    double s = 0, c = 0;   // slopes: x - s y - c > 0 (VSlope) or y - s x - c > 0 (HSlope)
};

inline bool inside(const Shape& sh, double x, double y) {
    switch (sh.kind) {
    case Shape::VLine: return x >= sh.a && x < sh.b;
    case Shape::HLine: return y >= sh.a && y < sh.b;
    case Shape::Circle: {
        const double dx = x - sh.cx, dy = y - sh.cy;
        return dx * dx + dy * dy < sh.a * sh.a;
    }
    case Shape::Ring: {
        auto inEll = [&](double A, double B) {
            const double dx = x - sh.cx, dy = y - sh.cy;
            const double lx = dx * std::cos(sh.phi) + dy * std::sin(sh.phi);
            const double ly = -dx * std::sin(sh.phi) + dy * std::cos(sh.phi);
            return (lx * lx) / (A * A) + (ly * ly) / (B * B) < 1.0;
        };
        return inEll(sh.a, sh.b) && !inEll(sh.ai, sh.bi);
    }
    case Shape::VSlope: return x - sh.s * y - sh.c > 0.0;
    case Shape::HSlope: return y - sh.s * x - sh.c > 0.0;
    }
    return false;
}

// An ellipse's radius from its centre along the angle theta (ring edges, the true curve for the ray measure).
inline double ellRadius(double A, double B, double phi, double theta) {
    const double ca = std::cos(theta - phi), sa = std::sin(theta - phi);
    return 1.0 / std::sqrt((ca / A) * (ca / A) + (sa / B) * (sa / B));
}

struct Img {
    int W = 0, H = 0;
    std::vector<double> v;   // display values, row-major
    double at(int x, int y) const {
        x = x < 0 ? 0 : x >= W ? W - 1 : x;
        y = y < 0 ? 0 : y >= H ? H - 1 : y;
        return v[size_t(y) * W + x];
    }
    // Bilinear, at a continuous output position (pixel X's centre is X + 0.5).
    double sample(double x, double y) const {
        const double sx = x - 0.5, sy = y - 0.5;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const double fx = sx - x0, fy = sy - y0;
        const double top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
        const double bot = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
        return top * (1 - fy) + bot * fy;
    }
};

// The 0.5-level crossing along a ray from (x0, y0) toward unit (dx, dy), near the true distance tt: rising (sign +1) or falling (-1).
inline bool rayCross(const Img& im, double x0, double y0, double dx, double dy, double tt, int sign, double& tMeas) {
    const double h = 0.02;
    double prevT = tt - 8.0, prevV = im.sample(x0 + dx * prevT, y0 + dy * prevT);
    double best = 1e18;
    bool found = false;
    for (double t = prevT + h; t <= tt + 8.0; t += h) {
        const double v = im.sample(x0 + dx * t, y0 + dy * t);
        const bool cross = sign > 0 ? (prevV < 0.5 && v >= 0.5) : (prevV >= 0.5 && v < 0.5);
        if (cross) {
            const double tc = prevT + h * (0.5 - prevV) / (v - prevV);
            if (std::fabs(tc - tt) < best) {
                best = std::fabs(tc - tt);
                tMeas = tc;
                found = true;
            }
        }
        prevT = t;
        prevV = v;
    }
    return found;
}

struct Dev {
    double rms = 0, mx = 0, stepRms = 0;
    int n = 0, misses = 0;
};

// The ray measure for a closed curve: outer edge (falling, hi inside to lo outside) and, for the ring, the inner edge (rising).
inline void rayDev(const Img& im, const Shape& sh, bool inner, Dev& d) {
    const int rays = 720;
    double sum = 0.0;
    for (int k = 0; k < rays; ++k) {
        const double th = 2.0 * 3.14159265358979323846 * k / rays;
        const double dx = std::cos(th), dy = std::sin(th);
        double tt;
        int sign;
        if (sh.kind == Shape::Circle) {
            tt = sh.a;
            sign = -1;
        } else {
            tt = inner ? ellRadius(sh.ai, sh.bi, sh.phi, th) : ellRadius(sh.a, sh.b, sh.phi, th);
            sign = inner ? +1 : -1;
        }
        double tm = 0.0;
        if (!rayCross(im, sh.cx, sh.cy, dx, dy, tt, sign, tm)) {
            ++d.misses;
            continue;
        }
        const double e = tm - tt;
        sum += e * e;
        d.mx = std::max(d.mx, std::fabs(e));
        ++d.n;
    }
    d.rms = d.n ? std::sqrt(sum / d.n) : 0.0;
}

// The slope measure: the rising 0.5-crossing in each row (VSlope) or column (HSlope), against the true position, and the step error.
inline void slopeDev(const Img& im, const Shape& sh, Dev& d) {
    const bool vert = sh.kind == Shape::VSlope;
    const int nL = vert ? im.H : im.W, nA = vert ? im.W : im.H;
    std::vector<double> xm(size_t(nL), std::nan(""));
    double sum = 0.0;
    for (int L = 40; L < nL - 40; ++L) {
        const double Lc = L + 0.5;
        const double truth = vert ? sh.c + sh.s * Lc : sh.c + sh.s * Lc;   // both: the crossing's A-position at line L's centre
        const int a0 = int(std::floor(truth)) - 12, a1 = int(std::floor(truth)) + 12;
        double best = 1e18, found = std::nan("");
        for (int A = a0; A < a1; ++A) {
            if (A < 0 || A + 1 >= nA) continue;
            const double vA = vert ? im.at(A, L) : im.at(L, A);
            const double vB = vert ? im.at(A + 1, L) : im.at(L, A + 1);
            if (vA < 0.5 && vB >= 0.5) {
                const double pos = (A + 0.5) + (0.5 - vA) / (vB - vA);
                if (std::fabs(pos - truth) < best) {
                    best = std::fabs(pos - truth);
                    found = pos;
                }
            }
        }
        if (std::isnan(found)) {
            ++d.misses;
            continue;
        }
        xm[size_t(L)] = found;
        const double e = found - truth;
        sum += e * e;
        d.mx = std::max(d.mx, std::fabs(e));
        ++d.n;
    }
    d.rms = d.n ? std::sqrt(sum / d.n) : 0.0;
    double ss = 0.0;
    int m = 0;
    for (int L = 40; L < nL - 41; ++L) {
        if (std::isnan(xm[size_t(L)]) || std::isnan(xm[size_t(L + 1)])) continue;
        const double st = (xm[size_t(L + 1)] - xm[size_t(L)]) - sh.s;
        ss += st * st;
        ++m;
    }
    d.stepRms = m ? std::sqrt(ss / m) : 0.0;
}

// Coverage error: over the partly covered output pixels (the analytic coverage strictly between 0.02 and 0.98), the mean absolute display
// error against the analytic display value lo + (hi - lo) cov.
inline double coverageError(const Img& im, const std::vector<double>& cov, double lo, double hi) {
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < cov.size(); ++i) {
        if (cov[i] <= 0.02 || cov[i] >= 0.98) continue;
        sum += std::fabs(im.v[i] - (lo + (hi - lo) * cov[i]));
        ++n;
    }
    return n ? sum / double(n) : 0.0;
}

// The cross-section through a line's middle, +-8 px: energy (sum of display above the background), centroid, peak; compared with the
// analytic cross-section. vert: a vertical line (a row's profile); otherwise a horizontal line (a column's profile).
struct LineStats {
    double energyRatio = 0, centroidShift = 0, peakRatio = 0;
};
inline LineStats lineStats(const Img& im, const std::vector<double>& cov, const Shape& sh, double lo, double hi) {
    const bool vert = sh.kind == Shape::VLine;
    const double mid = 0.5 * (sh.a + sh.b);
    const int c0 = int(std::floor(mid));
    const int fixed = vert ? im.H / 2 : im.W / 2;   // a row (vertical line) or a column (horizontal line), away from the frame edges
    double E = 0, Er = 0, cE = 0, cEr = 0, pk = 0, pkr = 0;
    for (int k = -8; k <= 8; ++k) {
        const int X = vert ? c0 + k : fixed;   // position along the profile
        const int Y = vert ? fixed : c0 + k;
        const size_t i = size_t(Y) * im.W + X;
        const double v = im.v[i] - lo, r = lo + (hi - lo) * cov[i] - lo;
        const double pos = (vert ? X : Y) + 0.5;
        E += v;
        Er += r;
        cE += pos * v;
        cEr += pos * r;
        if (v > pk) pk = v;
        if (r > pkr) pkr = r;
    }
    LineStats st;
    st.energyRatio = Er > 0 ? E / Er : 0;
    st.centroidShift = (E > 0 && Er > 0) ? cE / E - cEr / Er : 0;
    st.peakRatio = pkr > 0 ? pk / pkr : 0;
    return st;
}

}  // namespace shapesgpu

// The driver (2026-10-10, the shipped order: SMAA ahead of EASU below native). Validation first, then every shape through EASU alone and
// SMAA>EASU, one line per shape and variant. Gates (the bars the maintainer set on the jaggedness metric):
//  - axis-aligned lines (hard requirement): SMAA>EASU energy and peak equal EASU alone within 1 %, every line case;
//  - slope edges (near-vertical 0.03, near-horizontal 0.03, diagonal 0.6): SMAA>EASU row-to-row step RMS at most 0.8 of EASU alone;
//  - validation: the nearest-neighbour upscale scores jagged (circle RMS > 0.3, diagonal step RMS > 0.5); the analytic image scores near zero
//    (circle RMS < 0.1, diagonal RMS < 0.1 and step < 0.2);
//  - fail-if-skipped: SMAA forced off (the EASU-alone output) fails the slope gate.
// Reported, not gated: the circle and ring curve numbers (including the circle's outer-edge max), the 45 degree edge, coverage error.
inline void mapShapesGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using shapesgpu::Shape;
    using shapesgpu::Img;
    const int W = 1920, H = 1080, iW = 960, iH = 540;
    const double lo = 25.0 / 255.0, hi = 230.0 / 255.0;
    const double PI = 3.14159265358979323846;
    std::vector<Shape> list;
    auto addLine = [&](Shape::Kind k, const char* n, double a, double b) {
        Shape s;
        s.kind = k;
        s.name = n;
        s.a = a;
        s.b = b;
        list.push_back(s);
    };
    addLine(Shape::VLine, "vline 1px int", 600, 602);
    addLine(Shape::VLine, "vline 1px half", 601, 603);
    addLine(Shape::VLine, "vline 2px int", 600, 604);
    addLine(Shape::VLine, "vline 2px half", 601, 605);
    addLine(Shape::HLine, "hline 1px int", 400, 402);
    addLine(Shape::HLine, "hline 1px half", 401, 403);
    addLine(Shape::HLine, "hline 2px int", 400, 404);
    addLine(Shape::HLine, "hline 2px half", 401, 405);
    {
        Shape s;
        s.kind = Shape::Circle;
        s.name = "circle r200";
        s.a = 200;
        s.cx = 960.5;
        s.cy = 541.5;
        list.push_back(s);
    }
    {
        Shape s;
        s.kind = Shape::Ring;
        s.name = "ring 8px e0.31 tilt30";
        s.a = 400;
        s.b = 380;
        s.ai = 392;
        s.bi = 372;
        s.cx = 960.5;
        s.cy = 541.5;
        s.phi = 30.0 * PI / 180.0;
        list.push_back(s);
    }
    auto addSlope = [&](Shape::Kind k, const char* n, double sl, double c) {
        Shape s;
        s.kind = k;
        s.name = n;
        s.s = sl;
        s.c = c;
        list.push_back(s);
    };
    addSlope(Shape::VSlope, "diagonal 0.6", 0.6, 600.0);
    addSlope(Shape::VSlope, "45deg", 1.0, 600.0);
    addSlope(Shape::VSlope, "near-vert 0.03", 0.03, 600.6);
    addSlope(Shape::HSlope, "near-horiz 0.03", 0.03, 401.2);

    const char* variantNames[2] = {"EASU", "SMAA>EASU"};

    // The analytic coverage of an output image: 8 x 8 samples where the pixel's corners disagree, the whole pixel where they agree.
    auto coverage = [&](const Shape& sh) {
        std::vector<double> cov(size_t(W) * H);
        for (int Y = 0; Y < H; ++Y)
            for (int X = 0; X < W; ++X) {
                const bool c00 = shapesgpu::inside(sh, X, Y), c10 = shapesgpu::inside(sh, X + 1, Y);
                const bool c01 = shapesgpu::inside(sh, X, Y + 1), c11 = shapesgpu::inside(sh, X + 1, Y + 1);
                double v;
                if (c00 == c10 && c10 == c01 && c01 == c11) {
                    v = c00 ? 1.0 : 0.0;
                } else {
                    int in = 0;
                    for (int j = 0; j < 8; ++j)
                        for (int k = 0; k < 8; ++k)
                            if (shapesgpu::inside(sh, X + (k + 0.5) / 8.0, Y + (j + 0.5) / 8.0)) ++in;
                    v = in / 64.0;
                }
                cov[size_t(Y) * W + X] = v;
            }
        return cov;
    };
    auto measure = [&](const Shape& sh, const Img& im, const std::vector<double>& cov, char* buf, size_t n) {
        const double covErr = shapesgpu::coverageError(im, cov, lo, hi);
        if (sh.kind == Shape::VLine || sh.kind == Shape::HLine) {
            const shapesgpu::LineStats st = shapesgpu::lineStats(im, cov, sh, lo, hi);
            std::snprintf(buf, n, "energy %.3f centroid %+.3f peak %.3f | cov err %.5f", st.energyRatio, st.centroidShift, st.peakRatio, covErr);
        } else if (sh.kind == Shape::Circle) {
            shapesgpu::Dev o;
            shapesgpu::rayDev(im, sh, false, o);
            std::snprintf(buf, n, "outer RMS %.3f max %.3f misses %d | cov err %.5f", o.rms, o.mx, o.misses, covErr);
        } else if (sh.kind == Shape::Ring) {
            shapesgpu::Dev o, i2;
            shapesgpu::rayDev(im, sh, false, o);
            shapesgpu::rayDev(im, sh, true, i2);
            std::snprintf(buf, n, "outer RMS %.3f max %.3f | inner RMS %.3f max %.3f misses %d | cov err %.5f", o.rms, o.mx, i2.rms,
                          i2.mx, o.misses + i2.misses, covErr);
        } else {
            shapesgpu::Dev d;
            shapesgpu::slopeDev(im, sh, d);
            std::snprintf(buf, n, "RMS %.3f max %.3f step %.3f misses %d | cov err %.5f", d.rms, d.mx, d.stepRms, d.misses, covErr);
        }
    };

    // Validation: the metrics must score the nearest-neighbour upscale jagged and the analytic image near zero.
    {
        const Shape& vc = list[8];   // the circle
        const Shape& vs = list[10];  // the 0.6 diagonal
        // Circle: the outer-edge RMS of the nearest-neighbour and analytic images.
        {
            const std::vector<double> cov = coverage(vc);
            Img nn, an;
            nn.W = an.W = W;
            nn.H = an.H = H;
            nn.v.resize(cov.size());
            an.v.resize(cov.size());
            for (int Y = 0; Y < H; ++Y)
                for (int X = 0; X < W; ++X) {
                    const size_t i = size_t(Y) * W + X;
                    nn.v[i] = shapesgpu::inside(vc, 2 * (X / 2) + 1.0, 2 * (Y / 2) + 1.0) ? hi : lo;
                    an.v[i] = lo + (hi - lo) * cov[i];
                }
            shapesgpu::Dev dn, da;
            shapesgpu::rayDev(nn, vc, false, dn);
            shapesgpu::rayDev(an, vc, false, da);
            char b1[256], b2[256];
            measure(vc, nn, cov, b1, sizeof b1);
            measure(vc, an, cov, b2, sizeof b2);
            std::printf("map shapes validation %-16s nearest-neighbour: %s\n", vc.name, b1);
            std::printf("map shapes validation %-16s analytic:          %s\n", vc.name, b2);
            check(dn.rms > 0.3, "map shapes validation: the nearest-neighbour circle scores jagged (outer RMS > 0.3 px)");
            check(da.rms < 0.1, "map shapes validation: the analytic circle scores near zero (outer RMS < 0.1 px)");
        }
        // Diagonal: the row-to-row step of the nearest-neighbour and analytic images.
        {
            const std::vector<double> cov = coverage(vs);
            Img nn, an;
            nn.W = an.W = W;
            nn.H = an.H = H;
            nn.v.resize(cov.size());
            an.v.resize(cov.size());
            for (int Y = 0; Y < H; ++Y)
                for (int X = 0; X < W; ++X) {
                    const size_t i = size_t(Y) * W + X;
                    nn.v[i] = shapesgpu::inside(vs, 2 * (X / 2) + 1.0, 2 * (Y / 2) + 1.0) ? hi : lo;
                    an.v[i] = lo + (hi - lo) * cov[i];
                }
            shapesgpu::Dev dn, da;
            shapesgpu::slopeDev(nn, vs, dn);
            shapesgpu::slopeDev(an, vs, da);
            char b1[256], b2[256];
            measure(vs, nn, cov, b1, sizeof b1);
            measure(vs, an, cov, b2, sizeof b2);
            std::printf("map shapes validation %-16s nearest-neighbour: %s\n", vs.name, b1);
            std::printf("map shapes validation %-16s analytic:          %s\n", vs.name, b2);
            check(dn.stepRms > 0.5, "map shapes validation: the nearest-neighbour diagonal scores jagged (step RMS > 0.5 px)");
            check(da.rms < 0.1 && da.stepRms < 0.2, "map shapes validation: the analytic diagonal scores near zero (RMS < 0.1, step < 0.2)");
        }
    }

    for (const Shape& sh : list) {
        std::vector<uint8_t> px(size_t(iW) * iH * 4);
        for (int j = 0; j < iH; ++j)
            for (int i = 0; i < iW; ++i) {
                uint8_t* p = &px[(size_t(j) * iW + i) * 4];
                const bool on = shapesgpu::inside(sh, 2.0 * i + 1.0, 2.0 * j + 1.0);
                p[0] = p[1] = p[2] = uint8_t(on ? 230 : 25);
                p[3] = 255;
            }
        auto input = texture(device, iW, iH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, px.data(), iW * 4);
        if (!input) {
            check(false, "map shapes: the input texture is made");
            continue;
        }
        auto inputView = view(device, input.Get());
        const std::vector<double> cov = coverage(sh);
        shapesgpu::LineStats lineSt[2];
        shapesgpu::Dev slope[2];
        bool haveVariant[2] = {false, false};
        for (int variant = 0; variant < 2; ++variant) {
            edvr::FlatMonoResolveFrame f{};
            f.color = inputView.Get();
            f.renderWidth = iW;
            f.renderHeight = iH;
            f.outputWidth = W;
            f.outputHeight = H;
            f.mode = edvr::FlatMonoResolveMode::Dlss;
            f.hdr = false;
            f.reset = true;
            f.frame = 1;
            f.easu = true;
            f.smaa = variant == 1;
            ComPtr<ID3D11ShaderResourceView> out;
            const char* why = nullptr;
            const bool ok = edvr::flatMonoResolveSpatialFallback(device, context, f, out.GetAddressOf(), &why);
            const bool sm = edvr::flatMonoResolveLastSpatialWasSmaa();
            char msg[256];
            std::snprintf(msg, sizeof msg, "map shapes %s, %s: the variant runs and SMAA is honoured", sh.name, variantNames[variant]);
            check(ok && out && edvr::flatMonoResolveLastSpatialWasEasu() && sm == (variant == 1), msg);
            if (!ok || !out) {
                std::printf("  (%s, %s reason: %s)\n", sh.name, variantNames[variant], why ? why : "none");
                continue;
            }
            std::vector<float> r;
            UINT ow = 0, oh = 0;
            DXGI_FORMAT fm{};
            if (!mapeasugpu::readRgba(device, context, out.Get(), r, ow, oh, fm) || ow != UINT(W) || oh != UINT(H)) {
                std::snprintf(msg, sizeof msg, "map shapes %s, %s: the output reads back at 1920x1080", sh.name, variantNames[variant]);
                check(false, msg);
                continue;
            }
            Img im;
            im.W = W;
            im.H = H;
            im.v.resize(size_t(W) * H);
            for (size_t i = 0; i < im.v.size(); ++i) im.v[i] = r[i * 4];
            char buf[256];
            measure(sh, im, cov, buf, sizeof buf);
            std::printf("map shapes %-22s %-10s %s\n", sh.name, variantNames[variant], buf);
            haveVariant[variant] = true;
            if (sh.kind == Shape::VLine || sh.kind == Shape::HLine) lineSt[variant] = shapesgpu::lineStats(im, cov, sh, lo, hi);
            if (sh.kind == Shape::VSlope || sh.kind == Shape::HSlope) shapesgpu::slopeDev(im, sh, slope[variant]);
        }
        if (!haveVariant[0] || !haveVariant[1]) continue;
        char msg[256];
        if (sh.kind == Shape::VLine || sh.kind == Shape::HLine) {
            // Hard requirement: the line's energy and peak equal EASU's within 1 %.
            const double eRatio = lineSt[1].energyRatio / lineSt[0].energyRatio;
            const double pRatio = lineSt[1].peakRatio / lineSt[0].peakRatio;
            std::printf("map shapes %-22s line preservation: energy x%.4f, peak x%.4f (SMAA>EASU over EASU)\n", sh.name, eRatio, pRatio);
            std::snprintf(msg, sizeof msg, "map shapes %s: SMAA>EASU keeps the line's energy and peak within 1 %% of EASU (hard requirement)",
                          sh.name);
            check(std::fabs(eRatio - 1.0) <= 0.01 && std::fabs(pRatio - 1.0) <= 0.01, msg);
        }
        if (sh.kind == Shape::VSlope || sh.kind == Shape::HSlope) {
            const double sRatio = slope[0].stepRms > 0.0 ? slope[1].stepRms / slope[0].stepRms : 0.0;
            std::printf("map shapes %-22s step jaggedness: SMAA>EASU over EASU x%.3f (gate 0.8)\n", sh.name, sRatio);
            std::snprintf(msg, sizeof msg, "map shapes %s: SMAA>EASU row-to-row step RMS is at most 0.8 of EASU alone", sh.name);
            check(slope[0].stepRms > 0.0 && sRatio <= 0.8, msg);
            if (std::string(sh.name) == "near-vert 0.03") {
                // Fail-if-skipped: SMAA forced off is the EASU-alone output, which the same gate must reject (ratio 1.0 > 0.8).
                const double skippedRatio = slope[0].stepRms > 0.0 ? slope[0].stepRms / slope[0].stepRms : 0.0;
                std::printf("map shapes fail-if-skipped: SMAA forced off gives step ratio x%.3f, which the 0.8 gate rejects\n", skippedRatio);
                check(!(skippedRatio <= 0.8), "map shapes fail-if-skipped: SMAA forced off fails the slope gate");
            }
        }
    }
}
