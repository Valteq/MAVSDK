#include "log.h"
#include "mavsdk.h"
#include "plugins/ftp/ftp.h"
#include "plugins/ftp_server/ftp_server.h"
#include "fs_helpers.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <chrono>

using namespace mavsdk;

static constexpr double reduced_timeout_s = 0.1;

// TODO: make this compatible for Windows using GetTempPath2

static const fs::path temp_dir_provided = "/tmp/mavsdk_systemtest_temp_data/provided";

static const fs::path temp_dir = "folder";
static const fs::path temp_file = "file.bin";

TEST(SystemTest, FtpRemoveDir)
{
    Mavsdk mavsdk_groundstation;
    mavsdk_groundstation.set_configuration(
        Mavsdk::Configuration{Mavsdk::Configuration::UsageType::GroundStation});
    mavsdk_groundstation.set_timeout_s(reduced_timeout_s);

    Mavsdk mavsdk_autopilot;
    mavsdk_autopilot.set_configuration(
        Mavsdk::Configuration{Mavsdk::Configuration::UsageType::Autopilot});
    mavsdk_autopilot.set_timeout_s(reduced_timeout_s);

    ASSERT_EQ(mavsdk_groundstation.add_any_connection("udp://:17000"), ConnectionResult::Success);
    ASSERT_EQ(
        mavsdk_autopilot.add_any_connection("udp://127.0.0.1:17000"), ConnectionResult::Success);

    auto ftp_server = FtpServer{
        mavsdk_autopilot.server_component_by_type(Mavsdk::ServerComponentType::Autopilot)};

    auto maybe_system = mavsdk_groundstation.first_autopilot(10.0);
    ASSERT_TRUE(maybe_system);
    auto system = maybe_system.value();

    ASSERT_TRUE(system->has_autopilot());

    ASSERT_TRUE(reset_directories(temp_dir_provided / temp_dir));

    auto ftp = Ftp{system};

    // First we try to create a folder without the root directory set.
    // We expect that it does not exist as we don't have any permission.
    EXPECT_EQ(ftp.create_directory(temp_dir.string()), Ftp::Result::FileDoesNotExist);

    // Now we set the root dir and expect it to work.
    ftp_server.set_root_dir(temp_dir_provided.string());

    EXPECT_EQ(ftp.remove_directory(temp_dir.string()), Ftp::Result::Success);

    EXPECT_FALSE(file_exists(temp_dir_provided / temp_dir));
}

TEST(SystemTest, FtpRemoveDirNotEmpty)
{
    Mavsdk mavsdk_groundstation;
    mavsdk_groundstation.set_configuration(
        Mavsdk::Configuration{Mavsdk::Configuration::UsageType::GroundStation});
    mavsdk_groundstation.set_timeout_s(reduced_timeout_s);

    Mavsdk mavsdk_autopilot;
    mavsdk_autopilot.set_configuration(
        Mavsdk::Configuration{Mavsdk::Configuration::UsageType::Autopilot});
    mavsdk_autopilot.set_timeout_s(reduced_timeout_s);

    ASSERT_EQ(mavsdk_groundstation.add_any_connection("udp://:17000"), ConnectionResult::Success);
    ASSERT_EQ(
        mavsdk_autopilot.add_any_connection("udp://127.0.0.1:17000"), ConnectionResult::Success);

    auto ftp_server = FtpServer{
        mavsdk_autopilot.server_component_by_type(Mavsdk::ServerComponentType::Autopilot)};

    auto maybe_system = mavsdk_groundstation.first_autopilot(10.0);
    ASSERT_TRUE(maybe_system);
    auto system = maybe_system.value();

    ASSERT_TRUE(system->has_autopilot());

    ASSERT_TRUE(reset_directories(temp_dir_provided / temp_dir));
    ASSERT_TRUE(create_temp_file(temp_dir_provided / temp_dir / temp_file, 100));

    auto ftp = Ftp{system};

    // Now we set the root dir and expect it to work.
    ftp_server.set_root_dir(temp_dir_provided.string());

    // TODO: not sure if this is the correct error for "dir not empty"
    EXPECT_EQ(ftp.remove_directory(temp_dir.string()), Ftp::Result::ProtocolError);

    EXPECT_TRUE(file_exists(temp_dir_provided / temp_dir));
}