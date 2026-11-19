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

// ---- trend / change-point helpers (Phase 3) ----------------------------------------------------

TEST(stats_theil_sen) {
    std::vector<double> x, y;
    for (int i = 0; i < 20; ++i) {
        x.push_back(i);
        y.push_back(3.0 + 2.0 * i);
    }
    CHECK(near(*theil_sen_slope(x, y), 2.0));
    // 20 % gross outliers barely move it (least squares would).
    for (int i = 0; i < 20; i += 5) y[static_cast<std::size_t>(i)] = 1e6;
    CHECK(near(*theil_sen_slope(x, y), 2.0, 0.2));
    CHECK(!theil_sen_slope(std::vector<double>{1.0}, std::vector<double>{1.0}).has_value());
    CHECK(!theil_sen_slope(std::vector<double>{1.0, 1.0}, std::vector<double>{1.0, 5.0}).has_value());  // equal x
    // Long inputs are strided (bounded cost) and remain correct; scratch is reused.
    std::vector<double> lx, ly, scratch;
    for (int i = 0; i < 5000; ++i) {
        lx.push_back(i);
        ly.push_back(-0.5 * i + ((i % 2) ? 1.0 : -1.0));
    }
    CHECK(near(*theil_sen_slope(lx, ly, scratch), -0.5, 0.01));
    CHECK(scratch.size() <= kTheilSenMaxPoints * kTheilSenMaxPoints / 2);
    // NaN points are skipped.
    std::vector<double> nx{0, 1, 2, 3}, ny{0, std::nan(""), 2, 3};
    CHECK(near(*theil_sen_slope(nx, ny), 1.0));
}

TEST(stats_increasing_fraction_and_pearson) {
    CHECK(near(increasing_fraction(std::vector<double>{1, 2, 3, 4}), 1.0));
    CHECK(near(increasing_fraction(std::vector<double>{4, 3, 2, 1}), 0.0));
    CHECK(near(increasing_fraction(std::vector<double>{1, 2, 1, 2, 1}), 0.5));
    CHECK(near(increasing_fraction(std::vector<double>{1, 1, 1}), 0.0));        // flat is not increasing
    CHECK(near(increasing_fraction(std::vector<double>{1, 1.5, 2}, 1.0), 0.0)); // min_step
    CHECK(near(increasing_fraction(std::vector<double>{5}), 0.0));
    const std::vector<double> a{1, 2, 3, 4, 5};
    CHECK(near(*pearson(a, std::vector<double>{2, 4, 6, 8, 10}), 1.0));
    CHECK(near(*pearson(a, std::vector<double>{5, 4, 3, 2, 1}), -1.0));
    CHECK(!pearson(a, std::vector<double>{3, 3, 3, 3, 3}).has_value());
    CHECK(!pearson(std::vector<double>{1, 2}, std::vector<double>{1, 2}).has_value());
    const auto r = pearson(a, std::vector<double>{1, 3, 2, 5, 4});
    CHECK(r && *r > 0.5 && *r < 1.0);
}

TEST(stats_time_to_threshold) {
    CHECK(near(*time_to_threshold(80.0, 2.0, 100.0), 10.0));
    CHECK(near(*time_to_threshold(100.0, 2.0, 100.0), 0.0));
    CHECK(near(*time_to_threshold(120.0, -1.0, 100.0), 0.0));
    CHECK(!time_to_threshold(80.0, 0.0, 100.0).has_value());
    CHECK(!time_to_threshold(80.0, -1.0, 100.0).has_value());
    CHECK(!time_to_threshold(std::nan(""), 1.0, 100.0).has_value());
}

TEST(stats_page_hinkley) {
    CHECK_THROWS(PageHinkley(-1.0, 1.0));
    CHECK_THROWS(PageHinkley(0.1, 0.0));
    CHECK_THROWS(PageHinkley(0.1, 1.0, 0));
    // Stationary noise: no detection over a long run.
    PageHinkley ph(0.5, 20.0);
    unsigned s = 12345;
    auto noise = [&] {
        s = s * 1103515245u + 12345u;
        return static_cast<double>((s >> 16) % 1000) / 1000.0 - 0.5;  // [-0.5, 0.5)
    };
    int false_alarms = 0;
    for (int i = 0; i < 5000; ++i)
        if (ph.update(10.0 + noise())) ++false_alarms;
    CHECK_EQ(false_alarms, 0);
    // A sustained upward shift of +3 is detected within a few dozen samples.
    int detected_at = -1;
    for (int i = 0; i < 200 && detected_at < 0; ++i)
        if (ph.update(13.0 + noise())) detected_at = i;
    CHECK(detected_at >= 0 && detected_at < 40);
    CHECK_EQ(ph.samples(), 0u);  // restarted after the detection
    CHECK(!ph.update(std::nan("")));
}
