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

}  // namespace opensweep::stats
