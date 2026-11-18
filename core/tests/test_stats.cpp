#include <cmath>
#include <limits>
#include <vector>

#include "stats.hpp"
#include "test_framework.hpp"

using namespace opensweep::stats;

namespace {
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
}  // namespace

TEST(stats_ewma) {
    CHECK_THROWS(Ewma(0.0));
    CHECK_THROWS(Ewma(1.5));
    CHECK_THROWS(Ewma(-0.1));
    CHECK_THROWS(Ewma(std::nan("")));
    Ewma e(0.5);
    CHECK(!e.has_value());
    CHECK(near(e.update(10.0), 10.0));  // first sample initialises
    CHECK(near(e.update(20.0), 15.0));
    CHECK(near(e.update(20.0), 17.5));
    CHECK(near(e.update(std::numeric_limits<double>::infinity()), 17.5));  // ignored
    e.reset();
    CHECK(!e.has_value());
    Ewma one(1.0);
    one.update(3.0);
    CHECK(near(one.update(7.0), 7.0));
}

TEST(stats_median) {
    CHECK(!median(std::vector<double>{}).has_value());
    CHECK(near(*median(std::vector<double>{5.0}), 5.0));
    CHECK(near(*median(std::vector<double>{3.0, 1.0, 2.0}), 2.0));
    CHECK(near(*median(std::vector<double>{4.0, 1.0, 3.0, 2.0}), 2.5));
    CHECK(near(*median(std::vector<double>{1.0, std::nan(""), 3.0}), 2.0));  // NaN ignored
}

TEST(stats_mad) {
    // Classic example: {1,1,2,2,4,6,9}: median 2, deviations {1,1,0,0,2,4,7} -> MAD 1.
    CHECK(near(*mad(std::vector<double>{1, 1, 2, 2, 4, 6, 9}), 1.0));
    CHECK(near(*mad(std::vector<double>{5, 5, 5}), 0.0));
    CHECK(!mad(std::vector<double>{}).has_value());
}

TEST(stats_robust_z) {
    CHECK(near(*robust_z(4.0, 2.0, 1.0), 0.6745 * 2.0));
    CHECK(near(*robust_z(0.0, 2.0, 1.0), -0.6745 * 2.0));
    CHECK(!robust_z(4.0, 2.0, 0.0).has_value());  // undefined with zero spread
    CHECK(!robust_z(std::nan(""), 2.0, 1.0).has_value());
    const std::vector<double> window{10, 11, 9, 10, 12, 10, 9, 11};
    const auto z_normal = robust_z(10.5, window);
    const auto z_outlier = robust_z(40.0, window);
    CHECK(z_normal.has_value() && std::fabs(*z_normal) < 1.0);
    CHECK(z_outlier.has_value() && *z_outlier > 3.5);  // Iglewicz-Hoaglin outlier threshold
    CHECK(!robust_z(1.0, std::vector<double>{2, 2, 2}).has_value());
}
