#include "stats.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace opensweep::stats {

Ewma::Ewma(double alpha) : alpha_(alpha) {
    if (!(alpha > 0.0 && alpha <= 1.0)) throw std::invalid_argument("Ewma alpha must be in (0, 1]");
}

double Ewma::update(double sample) noexcept {
    if (!std::isfinite(sample)) return value_;
    if (!initialized_) {
        value_ = sample;
        initialized_ = true;
    } else {
        value_ += alpha_ * (sample - value_);
    }
    return value_;
}

namespace {

std::optional<double> median_inplace(std::vector<double>& v) {
    if (v.empty()) return std::nullopt;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double upper = v[mid];
    if (v.size() % 2 == 1) return upper;
    const double lower = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return lower + (upper - lower) / 2.0;
}

std::vector<double> finite_copy(std::span<const double> samples) {
    std::vector<double> v;
    v.reserve(samples.size());
    for (double d : samples) {
        if (std::isfinite(d)) v.push_back(d);
    }
    return v;
}

}  // namespace

std::optional<double> median(std::span<const double> samples) {
    auto v = finite_copy(samples);
    return median_inplace(v);
}

std::optional<double> mad(std::span<const double> samples) {
    auto v = finite_copy(samples);
    const auto m = median_inplace(v);
    if (!m) return std::nullopt;
    for (double& d : v) d = std::fabs(d - *m);
    return median_inplace(v);
}

std::optional<double> robust_z(double x, double median_value, double mad_value) noexcept {
    if (!std::isfinite(x) || !std::isfinite(median_value) || !std::isfinite(mad_value)) return std::nullopt;
    if (mad_value <= 0.0) return std::nullopt;
    return 0.6745 * (x - median_value) / mad_value;
}

std::optional<double> robust_z(double x, std::span<const double> samples) {
    const auto m = median(samples);
    const auto d = mad(samples);
    if (!m || !d) return std::nullopt;
    return robust_z(x, *m, *d);
}

std::optional<double> theil_sen_slope(std::span<const double> x, std::span<const double> y) {
    std::vector<double> scratch;
    return theil_sen_slope(x, y, scratch);
}

std::optional<double> theil_sen_slope(std::span<const double> x, std::span<const double> y,
                                      std::vector<double>& scratch) {
    double xs_buf[kTheilSenMaxPoints];
    double ys_buf[kTheilSenMaxPoints];
    std::size_t m = 0;
    {
        // Strided selection without heap allocation.
        const std::size_t n = std::min(x.size(), y.size());
        std::size_t usable = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (std::isfinite(x[i]) && std::isfinite(y[i])) ++usable;
        }
        if (usable < 2) return std::nullopt;
        const std::size_t keep = std::min(usable, kTheilSenMaxPoints);
        std::size_t u = 0;
        for (std::size_t i = 0; i < n && m < keep; ++i) {
            if (!(std::isfinite(x[i]) && std::isfinite(y[i]))) continue;
            const std::size_t target = (m * (usable - 1) + (keep - 1) / 2) / (keep - 1);
            if (u == target) {
                xs_buf[m] = x[i];
                ys_buf[m] = y[i];
                ++m;
            }
            ++u;
        }
    }
    scratch.clear();
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = i + 1; j < m; ++j) {
            const double dx = xs_buf[j] - xs_buf[i];
            if (dx == 0.0) continue;
            const double s = (ys_buf[j] - ys_buf[i]) / dx;
            if (std::isfinite(s)) scratch.push_back(s);
        }
    }
    return median_inplace(scratch);
}

double increasing_fraction(std::span<const double> y, double min_step) noexcept {
    std::size_t steps = 0;
    std::size_t up = 0;
    bool have_prev = false;
    double prev = 0.0;
    for (double v : y) {
        if (!std::isfinite(v)) continue;
        if (have_prev) {
            ++steps;
            if (v - prev > min_step) ++up;
        }
        prev = v;
        have_prev = true;
    }
    return steps == 0 ? 0.0 : static_cast<double>(up) / static_cast<double>(steps);
}

std::optional<double> pearson(std::span<const double> x, std::span<const double> y) noexcept {
    const std::size_t n = std::min(x.size(), y.size());
    double sx = 0, sy = 0;
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) continue;
        sx += x[i];
        sy += y[i];
        ++k;
    }
    if (k < 3) return std::nullopt;
    const double mx = sx / static_cast<double>(k);
    const double my = sy / static_cast<double>(k);
    double cxy = 0, cxx = 0, cyy = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) continue;
        const double dx = x[i] - mx;
        const double dy = y[i] - my;
        cxy += dx * dy;
        cxx += dx * dx;
        cyy += dy * dy;
    }
    if (cxx <= 0.0 || cyy <= 0.0) return std::nullopt;
    const double r = cxy / std::sqrt(cxx * cyy);
    if (!std::isfinite(r)) return std::nullopt;
    return std::clamp(r, -1.0, 1.0);
}

std::optional<double> time_to_threshold(double current, double slope_per_unit, double threshold) noexcept {
    if (!std::isfinite(current) || !std::isfinite(slope_per_unit) || !std::isfinite(threshold)) return std::nullopt;
    if (current >= threshold) return 0.0;
    if (slope_per_unit <= 0.0) return std::nullopt;
    return (threshold - current) / slope_per_unit;
}

PageHinkley::PageHinkley(double delta, double lambda, std::size_t min_samples)
    : delta_(delta), lambda_(lambda), min_samples_(min_samples) {
    if (!(delta >= 0.0) || !std::isfinite(delta) || !(lambda > 0.0) || !std::isfinite(lambda) || min_samples == 0)
        throw std::invalid_argument("PageHinkley: delta >= 0, lambda > 0, min_samples >= 1 required");
}

bool PageHinkley::update(double x) noexcept {
    if (!std::isfinite(x)) return false;
    ++n_;
    mean_ += (x - mean_) / static_cast<double>(n_);
    cum_ += x - mean_ - delta_;
    min_cum_ = std::min(min_cum_, cum_);
    if (n_ >= min_samples_ && cum_ - min_cum_ > lambda_) {
        reset();
        return true;
    }
    return false;
}

void PageHinkley::reset() noexcept {
    n_ = 0;
    mean_ = 0.0;
    cum_ = 0.0;
    min_cum_ = 0.0;
}

}  // namespace opensweep::stats
