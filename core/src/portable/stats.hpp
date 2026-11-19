// Small, allocation-light statistics helpers used by later phases (anomaly scoring,
// smoothing of noisy counters). Pure functions; no Windows dependencies.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace opensweep::stats {

// Exponentially weighted moving average. The first sample initialises the average.
class Ewma {
public:
    // alpha in (0, 1]; higher reacts faster. Throws std::invalid_argument otherwise.
    explicit Ewma(double alpha);

    // Adds a sample and returns the new average. Non-finite samples are ignored.
    double update(double sample) noexcept;
    bool has_value() const noexcept { return initialized_; }
    double value() const noexcept { return value_; }
    double alpha() const noexcept { return alpha_; }
    void reset() noexcept {
        initialized_ = false;
        value_ = 0.0;
    }

private:
    double alpha_;
    double value_ = 0.0;
    bool initialized_ = false;
};

// Median of the samples (mean of the two middle values for even counts).
// Returns nullopt for an empty input. Non-finite values are ignored.
std::optional<double> median(std::span<const double> samples);

// Median absolute deviation (unscaled): median(|x_i - median(x)|).
std::optional<double> mad(std::span<const double> samples);

// Robust (modified) z-score after Iglewicz & Hoaglin: 0.6745 * (x - median) / MAD.
// Returns nullopt when mad <= 0 or inputs are not finite (score undefined).
std::optional<double> robust_z(double x, double median_value, double mad_value) noexcept;

// Convenience: robust z-score of x against a sample window.
std::optional<double> robust_z(double x, std::span<const double> samples);

// ---------------------------------------------------------------------------------------------
// Trend / change-point helpers (Phase 3, SweepGuard).

// Theil-Sen slope estimator: the median of the pairwise slopes (y_j - y_i) / (x_j - x_i), i < j.
// Robust to up to ~29% outliers. Pairs with equal x and non-finite values are skipped. Returns
// nullopt with fewer than two usable points. Inputs longer than `kTheilSenMaxPoints` are
// evaluated on an evenly strided subset so the cost stays bounded (O(k^2) with k <= 64).
// The overload taking `scratch` reuses that buffer (no allocation once it has grown).
inline constexpr std::size_t kTheilSenMaxPoints = 64;
std::optional<double> theil_sen_slope(std::span<const double> x, std::span<const double> y);
std::optional<double> theil_sen_slope(std::span<const double> x, std::span<const double> y,
                                      std::vector<double>& scratch);

// Fraction (0..1) of consecutive steps that strictly increase by more than `min_step`.
// Returns 0 for fewer than two finite values. Used as a monotonicity check next to the slope.
double increasing_fraction(std::span<const double> y, double min_step = 0.0) noexcept;

// Pearson correlation coefficient of two equally long series (pairs with a non-finite member are
// skipped). nullopt when fewer than 3 pairs remain or either series has zero variance.
std::optional<double> pearson(std::span<const double> x, std::span<const double> y) noexcept;

// Linear extrapolation: time until `current` reaches `threshold` growing at `slope_per_unit`.
// Returns 0 when already at/above the threshold, nullopt when the slope is <= 0 or not finite
// (never reached). The unit of the result is the unit of the slope's denominator.
std::optional<double> time_to_threshold(double current, double slope_per_unit, double threshold) noexcept;

// Page-Hinkley change-point detector for an upward shift of the mean. `delta` is the tolerated
// drift per sample (same unit as the samples), `lambda` the detection threshold on the cumulative
// deviation. After a detection the detector restarts from the next sample. Non-finite samples are
// ignored. O(1) memory.
class PageHinkley {
public:
    // delta >= 0, lambda > 0, min_samples >= 1; throws std::invalid_argument otherwise.
    PageHinkley(double delta, double lambda, std::size_t min_samples = 5);
    // Returns true when an upward change is detected at this sample.
    bool update(double x) noexcept;
    void reset() noexcept;
    std::size_t samples() const noexcept { return n_; }
    double mean() const noexcept { return mean_; }
    // Current statistic m_t - min(m); compare against lambda.
    double statistic() const noexcept { return cum_ - min_cum_; }

private:
    double delta_;
    double lambda_;
    std::size_t min_samples_;
    std::size_t n_ = 0;
    double mean_ = 0.0;
    double cum_ = 0.0;
    double min_cum_ = 0.0;
};

}  // namespace opensweep::stats
