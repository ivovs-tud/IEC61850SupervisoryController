#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#include "sc/application/YawLut.hpp"

namespace {

class TemporaryCsv {
public:
    explicit TemporaryCsv(const std::string& contents) {
        static std::atomic<unsigned long> nextId{0};
        path_ = std::filesystem::temp_directory_path() /
                ("sc-yaw-lut-" + std::to_string(std::random_device{}()) + "-" +
                 std::to_string(nextId.fetch_add(1)) + ".csv");

        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to create temporary yaw LUT CSV");
        }
        file << contents;
        if (!file) {
            throw std::runtime_error("Failed to write temporary yaw LUT CSV");
        }
    }

    ~TemporaryCsv() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryCsv(const TemporaryCsv&) = delete;
    TemporaryCsv& operator=(const TemporaryCsv&) = delete;

    std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

std::string gridWithHeader(const std::string& header) {
    return header +
           "0,0,0,0\n"
           "0,20,20,40\n"
           "10,0,10,20\n"
           "10,20,30,60\n";
}

void requireSetpoints(const sc::application::YawLut::TurbineYawSetpoints& actual,
                      float first,
                      float second) {
    REQUIRE(actual.size() == 2);
    REQUIRE(actual[0] == Catch::Approx(first));
    REQUIRE(actual[1] == Catch::Approx(second));
}

} // namespace

TEST_CASE("yaw LUT accepts both documented header styles") {
    SECTION("short wind fields and turbine columns") {
        TemporaryCsv file(gridWithHeader("ws,wd,WT1,WT2\n"));
        const sc::application::YawLut lut(file.path());
        requireSetpoints(lut.lookup(10.0F, 20.0F), 30.0F, 60.0F);
    }

    SECTION("bin fields and yaw columns") {
        TemporaryCsv file(gridWithHeader(" ws_bin , wd_bin , yaw1 , yaw2 \r\n"));
        const sc::application::YawLut lut(file.path());
        requireSetpoints(lut.lookup(0.0F, 0.0F), 0.0F, 0.0F);
    }
}

TEST_CASE("yaw LUT rejects malformed CSV") {
    SECTION("invalid header") {
        TemporaryCsv file(gridWithHeader("ws,bearing,WT1,WT2\n"));
        REQUIRE_THROWS_AS(sc::application::YawLut(file.path()), std::runtime_error);
    }

    SECTION("invalid numeric value") {
        TemporaryCsv file(
            "ws,wd,WT1\n"
            "0,0,invalid\n"
            "0,20,1\n"
            "10,0,2\n"
            "10,20,3\n");
        REQUIRE_THROWS_AS(sc::application::YawLut(file.path()), std::runtime_error);
    }

    SECTION("too few columns") {
        TemporaryCsv file("ws,wd\n0,0\n");
        REQUIRE_THROWS_AS(sc::application::YawLut(file.path()), std::runtime_error);
    }
}

TEST_CASE("yaw LUT rejects incomplete grids") {
    TemporaryCsv file(
        "ws,wd,WT1\n"
        "0,0,0\n"
        "0,20,20\n"
        "10,0,10\n");

    REQUIRE_THROWS_AS(sc::application::YawLut(file.path()), std::runtime_error);
}

TEST_CASE("yaw LUT rejects duplicate bin combinations") {
    TemporaryCsv file(
        "ws,wd,WT1\n"
        "0,0,0\n"
        "0,0,1\n"
        "0,20,20\n"
        "10,0,10\n"
        "10,20,30\n");

    REQUIRE_THROWS_AS(sc::application::YawLut(file.path()), std::runtime_error);
}

TEST_CASE("yaw LUT clamps values outside the grid") {
    TemporaryCsv file(gridWithHeader("ws,wd,WT1,WT2\n"));
    const sc::application::YawLut lut(file.path());

    requireSetpoints(lut.lookup(-5.0F, -10.0F), 0.0F, 0.0F);
    requireSetpoints(lut.lookup(15.0F, 30.0F), 30.0F, 60.0F);
    requireSetpoints(lut.lookup(-5.0F, 30.0F), 20.0F, 40.0F);
}

TEST_CASE("yaw LUT returns exact bin values") {
    TemporaryCsv file(gridWithHeader("ws,wd,WT1,WT2\n"));
    const sc::application::YawLut lut(file.path());

    requireSetpoints(lut.lookup(0.0F, 20.0F), 20.0F, 40.0F);
    requireSetpoints(lut.lookup(10.0F, 0.0F), 10.0F, 20.0F);
}

TEST_CASE("yaw LUT performs bilinear interpolation") {
    TemporaryCsv file(gridWithHeader("ws,wd,WT1,WT2\n"));
    const sc::application::YawLut lut(file.path());

    requireSetpoints(lut.lookup(5.0F, 10.0F), 15.0F, 30.0F);
    requireSetpoints(lut.lookup(2.5F, 5.0F), 7.5F, 15.0F);
}

TEST_CASE("yaw LUT preserves and validates turbine column count") {
    SECTION("lookup contains one value per turbine column") {
        TemporaryCsv file(gridWithHeader("ws,wd,WT1,WT2\n"));
        const sc::application::YawLut lut(file.path());
        REQUIRE(lut.lookup(5.0F, 10.0F).size() == 2);
    }

    SECTION("rows must have a consistent number of turbine columns") {
        TemporaryCsv file(
            "ws,wd,WT1,WT2\n"
            "0,0,0,0\n"
            "0,20,20\n"
            "10,0,10,20\n"
            "10,20,30,60\n");
        REQUIRE_THROWS_AS(sc::application::YawLut(file.path()), std::runtime_error);
    }
}
