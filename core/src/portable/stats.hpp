// Small, allocation-light statistics helpers used by later phases (anomaly scoring,
// smoothing of noisy counters). Pure functions; no Windows dependencies.
#pragma once

#include <cstddef>
#include <optional>
#include <span>

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

}  // namespace opensweep::stats
