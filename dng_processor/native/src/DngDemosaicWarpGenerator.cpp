#include "Halide.h"

using namespace Halide;

namespace {

Expr positive_modulo(Expr v, Expr m) {
    return ((v % m) + m) % m;
}

Expr map_repeat_coord(Expr coord, Expr size) {
    Expr repeat = min(Expr(2), size);
    Expr start = size - repeat;
    return select(coord < 0,
                  positive_modulo(coord, repeat),
                  coord >= size,
                  start + positive_modulo(coord - start, repeat),
                  coord);
}

Expr avg2_u16(Expr a, Expr b) {
    return cast<uint16_t>((cast<uint32_t>(a) + cast<uint32_t>(b) + cast<uint32_t>(1)) >> 1);
}

Expr avg4_u16(Expr a, Expr b, Expr c, Expr d) {
    Expr total = cast<uint32_t>(a) + cast<uint32_t>(b) + cast<uint32_t>(c) + cast<uint32_t>(d);
    return cast<uint16_t>((total + cast<uint32_t>(2)) >> 2);
}

Expr cubic_weight(Expr x) {
    const float a = -0.75f;
    x = abs(x);
    return select(x >= 2.0f, 0.0f,
                  x >= 1.0f, (((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a),
                  (((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f));
}

constexpr int kResampleSubsampleBits2D = 5;
constexpr int kResampleSubsampleCount2D = 1 << kResampleSubsampleBits2D;

}  // namespace

class DngDemosaicWarp : public Halide::Generator<DngDemosaicWarp> {
public:
    GeneratorParam<bool> fast_codegen{"fast_codegen", true};

    Input<Buffer<uint16_t>> src{"src", 2};      // x, y Bayer CFA
    Input<Buffer<float>> rad{"rad", 2};         // coeff, plane
    Input<Buffer<float>> tan{"tan", 2};         // coeff, plane
    Input<Buffer<int32_t>> tile_bounds{"tile_bounds", 3};  // bound(4), tile_x, tile_y
    Input<int32_t> planes{"planes"};
    Input<float> center_x{"center_x"};
    Input<float> center_y{"center_y"};
    Input<float> norm_radius{"norm_radius"};
    Input<float> inv_norm_radius{"inv_norm_radius"};
    Input<float> pixel_scale_v{"pixel_scale_v"};
    Input<float> pixel_scale_v_inv{"pixel_scale_v_inv"};
    Input<int32_t> is_rad_nop_all{"is_rad_nop_all"};
    Input<int32_t> is_tan_nop_all{"is_tan_nop_all"};
    Input<int32_t> tile_width{"tile_width"};
    Input<int32_t> tile_height{"tile_height"};

    Output<Buffer<uint16_t>> dst{"dst", 3};

    Func demosaic{"demosaic"};
    Func rgb_clamped{"rgb_clamped"};

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        tile_bounds.dim(0).set_bounds(0, 4);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();

        auto cfa_sample = [&](Expr sx, Expr sy) {
            Expr mx = map_repeat_coord(sx, width);
            Expr my = map_repeat_coord(sy, height);
            return src(mx, my);
        };

        Expr center = cfa_sample(x, y);
        Expr n = cfa_sample(x, y - 1);
        Expr s = cfa_sample(x, y + 1);
        Expr w = cfa_sample(x - 1, y);
        Expr e = cfa_sample(x + 1, y);
        Expr nw = cfa_sample(x - 1, y - 1);
        Expr ne = cfa_sample(x + 1, y - 1);
        Expr sw = cfa_sample(x - 1, y + 1);
        Expr se = cfa_sample(x + 1, y + 1);

        Expr even_row = (y % 2) == 0;
        Expr even_col = (x % 2) == 0;
        Expr red_site = even_row && even_col;
        Expr blue_site = !even_row && !even_col;
        Expr green_on_red_row = even_row && !even_col;

        Expr r = select(red_site,
                        center,
                        blue_site,
                        avg4_u16(nw, ne, sw, se),
                        green_on_red_row,
                        avg2_u16(w, e),
                        avg2_u16(n, s));

        Expr g = select(red_site || blue_site,
                        avg4_u16(n, s, w, e),
                        center);

        Expr b = select(red_site,
                        avg4_u16(nw, ne, sw, se),
                        blue_site,
                        center,
                        green_on_red_row,
                        avg2_u16(n, s),
                        avg2_u16(w, e));

        demosaic(x, y, c) = select(c == 0, r, c == 1, g, b);

        Expr plane = select(planes <= 1, 0, select(c < planes, c, 0));
        plane = clamp(plane, 0, rad.dim(1).extent() - 1);

        Expr diff_x = cast<float>(x) - center_x;
        Expr diff_y = cast<float>(y) - center_y;
        Expr diff_norm_x = diff_x * inv_norm_radius;
        Expr diff_norm_y = diff_y * inv_norm_radius;
        Expr diff_scaled_x = diff_norm_x;
        Expr diff_scaled_y = diff_norm_y * pixel_scale_v;
        Expr rr = min(diff_scaled_x * diff_scaled_x + diff_scaled_y * diff_scaled_y, 1.0f);

        Expr ratio = rad(0, plane) + rr * (rad(1, plane) + rr * (rad(2, plane) + rr * rad(3, plane)));
        Expr tan_v = tan(0, plane) * (rr + 2.0f * diff_scaled_y * diff_scaled_y) +
                     (2.0f * tan(1, plane) * diff_scaled_x * diff_scaled_y);
        Expr tan_h = tan(1, plane) * (rr + 2.0f * diff_scaled_x * diff_scaled_x) +
                     (2.0f * tan(0, plane) * diff_scaled_x * diff_scaled_y);

        Expr src_x_rad = center_x + diff_x * ratio;
        Expr src_y_rad = center_y + diff_y * ratio;
        Expr src_x_tan = cast<float>(x) + norm_radius * tan_h;
        Expr src_y_tan = cast<float>(y) + norm_radius * tan_v * pixel_scale_v_inv;
        Expr src_x_both = center_x + norm_radius * (diff_norm_x * ratio + tan_h);
        Expr src_y_both = center_y + norm_radius * (diff_norm_y * ratio + tan_v * pixel_scale_v_inv);

        Expr src_x = select(is_tan_nop_all != 0,
                            src_x_rad,
                            is_rad_nop_all != 0,
                            src_x_tan,
                            src_x_both);
        Expr src_y = select(is_tan_nop_all != 0,
                            src_y_rad,
                            is_rad_nop_all != 0,
                            src_y_tan,
                            src_y_both);

        Expr src_x_floor = floor(src_x);
        Expr src_y_floor = floor(src_y);

        Expr frac_x_idx = clamp(cast<int>(floor((src_x - src_x_floor) * kResampleSubsampleCount2D)),
                                0,
                                kResampleSubsampleCount2D - 1);
        Expr frac_y_idx = clamp(cast<int>(floor((src_y - src_y_floor) * kResampleSubsampleCount2D)),
                                0,
                                kResampleSubsampleCount2D - 1);

        Expr base_x = cast<int>(src_x_floor) - 1;
        Expr base_y = cast<int>(src_y_floor) - 1;

        Expr safe_tile_w = max(tile_width, 1);
        Expr safe_tile_h = max(tile_height, 1);
        Expr tile_x = clamp(x / safe_tile_w, 0, tile_bounds.dim(1).extent() - 1);
        Expr tile_y = clamp(y / safe_tile_h, 0, tile_bounds.dim(2).extent() - 1);

        Expr min_base_x = tile_bounds(0, tile_x, tile_y);
        Expr max_base_x = tile_bounds(1, tile_x, tile_y);
        Expr min_base_y = tile_bounds(2, tile_x, tile_y);
        Expr max_base_y = tile_bounds(3, tile_x, tile_y);

        Expr clipped_x = base_x < min_base_x || base_x > max_base_x;
        Expr clipped_y = base_y < min_base_y || base_y > max_base_y;

        Expr base_x_clamped = clamp(base_x, min_base_x, max_base_x);
        Expr base_y_clamped = clamp(base_y, min_base_y, max_base_y);

        Expr frac_x = select(clipped_x,
                             0.0f,
                             cast<float>(frac_x_idx) / kResampleSubsampleCount2D);
        Expr frac_y = select(clipped_y,
                             0.0f,
                             cast<float>(frac_y_idx) / kResampleSubsampleCount2D);

        Expr w0x = cubic_weight((-1.0f) - frac_x);
        Expr w1x = cubic_weight(0.0f - frac_x);
        Expr w2x = cubic_weight(1.0f - frac_x);
        Expr w3x = cubic_weight(2.0f - frac_x);
        Expr sumx = w0x + w1x + w2x + w3x;
        w0x /= sumx;
        w1x /= sumx;
        w2x /= sumx;
        w3x /= sumx;

        Expr w0y = cubic_weight((-1.0f) - frac_y);
        Expr w1y = cubic_weight(0.0f - frac_y);
        Expr w2y = cubic_weight(1.0f - frac_y);
        Expr w3y = cubic_weight(2.0f - frac_y);
        Expr sumy = w0y + w1y + w2y + w3y;
        w0y /= sumy;
        w1y /= sumy;
        w2y /= sumy;
        w3y /= sumy;

        rgb_clamped(x, y, c) = cast<float>(demosaic(clamp(x, 0, src.dim(0).extent() - 1),
                                                    clamp(y, 0, src.dim(1).extent() - 1),
                                                    clamp(c, 0, 2)));

        auto sample_row = [&](Expr yy, Expr wx0, Expr wx1, Expr wx2, Expr wx3) {
            return wx0 * rgb_clamped(base_x_clamped + 0, yy, c) +
                   wx1 * rgb_clamped(base_x_clamped + 1, yy, c) +
                   wx2 * rgb_clamped(base_x_clamped + 2, yy, c) +
                   wx3 * rgb_clamped(base_x_clamped + 3, yy, c);
        };

        Expr row0 = sample_row(base_y_clamped + 0, w0x, w1x, w2x, w3x);
        Expr row1 = sample_row(base_y_clamped + 1, w0x, w1x, w2x, w3x);
        Expr row2 = sample_row(base_y_clamped + 2, w0x, w1x, w2x, w3x);
        Expr row3 = sample_row(base_y_clamped + 3, w0x, w1x, w2x, w3x);

        Expr value = w0y * row0 + w1y * row1 + w2y * row2 + w3y * row3;
        dst(x, y, c) = cast<uint16_t>(clamp(value + 0.5f, 0.0f, 65535.0f));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            if (fast_codegen) {
                dst.bound(c, 0, 3)
                   .reorder(c, x, y)
                   .gpu_tile(x, y, xo, yo, xi, yi, 8, 8);
            } else {
                dst.bound(c, 0, 3)
                   .reorder(c, x, y)
                   .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
                   .unroll(c);
            }
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 4);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DngDemosaicWarp, dng_demosaic_warp)
