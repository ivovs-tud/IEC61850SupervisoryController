#include "sc/util/Angles.hpp"
#include "sc/util/Statistics.hpp"
#include "sc/util/Time.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("angles are normalized and rounded to one rotation") {
    REQUIRE(sc::util::normalizeAngleDegrees(-1.0F) == Catch::Approx(359.0F));
    REQUIRE(sc::util::normalizeAngleDegrees(721.0F) == Catch::Approx(1.0F));
    REQUIRE(sc::util::roundAngleDegrees(359.6F) == Catch::Approx(0.0F));
    REQUIRE(sc::util::roundAngleDegrees(-0.6F) == Catch::Approx(359.0F));
}

TEST_CASE("angular differences follow the shortest path") {
    REQUIRE(sc::util::signedAngleDifferenceDegrees(359.0F, 1.0F) == Catch::Approx(2.0F));
    REQUIRE(sc::util::signedAngleDifferenceDegrees(1.0F, 359.0F) == Catch::Approx(-2.0F));
    REQUIRE(sc::util::angularDistanceDegrees(1.0F, 359.0F) == Catch::Approx(2.0F));
}

TEST_CASE("circular statistics handle wraparound") {
    const std::vector<double> angles{359.0, 1.0, 180.0};
    REQUIRE(sc::util::circularMeanDegrees(angles, 2) == Catch::Approx(0.0F).margin(0.001F));
    REQUIRE(sc::util::angularSpreadDegrees({359.0, 1.0}) == Catch::Approx(1.0).margin(0.001));
}

TEST_CASE("angles blend and move across zero") {
    REQUIRE(sc::util::blendAnglesDegrees(350.0F, 10.0F, 0.5F) == Catch::Approx(0.0F));
    REQUIRE(sc::util::moveTowardsAngleDegrees(350.0F, 10.0F, 5.0F, 1.0F) == Catch::Approx(355.0F));
    REQUIRE(sc::util::moveTowardsAngleDegrees(10.0F, 350.0F, 5.0F, 1.0F) == Catch::Approx(5.0F));
}

TEST_CASE("statistics handle empty and populated samples") {
    REQUIRE(sc::util::mean({}) == 0.0);
    REQUIRE(sc::util::median({}) == 0.0);
    REQUIRE(sc::util::valueRange({}) == 0.0);
    REQUIRE(sc::util::mean({1.0, 2.0, 3.0}) == Catch::Approx(2.0));
    REQUIRE(sc::util::median({3.0, 1.0, 2.0}) == Catch::Approx(2.0));
    REQUIRE(sc::util::medianAbsoluteDeviation({1.0, 2.0, 10.0}, 2.0) == Catch::Approx(1.0));
    REQUIRE(sc::util::valueRange({7.0, -2.0, 4.0}) == Catch::Approx(9.0));
}

TEST_CASE("median preserves the existing upper-middle behavior") {
    REQUIRE(sc::util::median({1.0, 2.0, 10.0, 20.0}) == Catch::Approx(10.0));
}

TEST_CASE("timestamp recency handles missing future and expired measurements") {
    REQUIRE_FALSE(sc::util::isTimestampRecent(0, 1000, 100));
    REQUIRE(sc::util::isTimestampRecent(950, 1000, 100));
    REQUIRE(sc::util::isTimestampRecent(1050, 1000, 100));
    REQUIRE_FALSE(sc::util::isTimestampRecent(899, 1000, 100));
}
