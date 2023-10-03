//
// Example to connect multiple vehicles and make them take off and land in parallel.
//./multiple_drones udp://:14540 udp://:14541
//

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <cstdint>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

using namespace mavsdk;
using namespace std::this_thread;
using namespace std::chrono;

static void takeoff_and_land(std::shared_ptr<System> system);

void usage(const std::string& bin_name)
{
    std::cerr << "Usage : " << bin_name << " <connection_url_1> [<connection_url_2> ...]\n"
              << "Connection URL format should be :\n"
              << " For TCP : tcp://[server_host][:server_port]\n"
              << " For UDP : udp://[bind_host][:bind_port]\n"
              << " For Serial : serial:///path/to/serial/dev[:baudrate]\n"
              << "For example, to connect to the simulator use URL: udp://:14540\n";
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Please specify connection\n";
        usage(argv[0]);
        return 1;
    }

    Mavsdk mavsdk;

    size_t total_udp_ports = argc - 1;

    // the first drone
    ConnectionResult connection_result = mavsdk.add_any_connection(argv[1]);
    if (connection_result != ConnectionResult::Success) {
        std::cerr << "Connection error: " << connection_result << '\n';
        return 1;
    }

    std::atomic<size_t> num_systems_discovered{0};

    std::cout << "Waiting to discover system...\n";
    mavsdk.subscribe_on_new_system([&mavsdk, &num_systems_discovered]() {
        const auto systems = mavsdk.systems();

        if (systems.size() > num_systems_discovered) {
            std::cout << "Discovered system\n";
            num_systems_discovered = systems.size();
        }
    });

    // We usually receive heartbeats at 1Hz, therefore we should find a system after around 2
    // seconds.
    sleep_for(seconds(2));

    int current_num_systems_discovered = num_systems_discovered;

    std::vector<std::thread> threads;

    for (auto system : mavsdk.systems()) {
        std::thread t(&takeoff_and_land, std::ref(system));
        threads.push_back(std::move(t));
        sleep_for(seconds(1));
    }

    for (auto& t : threads) {
        t.join();
    }


    // the second drone
    ConnectionResult connection_result_2 = mavsdk.add_any_connection(argv[2]);
    if (connection_result_2 != ConnectionResult::Success) {
        std::cerr << "Connection error for new URL: " << connection_result_2 << '\n';
    } else {
        // Wait for the new system to be discovered
        sleep_for(seconds(2));

        auto new_systems = mavsdk.systems();
        if (new_systems.size() > current_num_systems_discovered) {
            std::cout << "New system discovered, starting takeoff_and_land...\n";

            // Launch a new thread for the takeoff_and_land operation for the new drone
            std::thread new_drone_thread(&takeoff_and_land, std::ref(new_systems.back()));
            new_drone_thread.join(); // Optionally, you can join immediately or store the thread to join later
        } else {
            std::cerr << "Failed to discover the new system.\n";
        }
    }

    return 0;
}

void takeoff_and_land(std::shared_ptr<System> system)
{
    auto telemetry = Telemetry{system};
    auto action = Action{system};

    // We want to listen to the altitude of the drone at 1 Hz.
    const Telemetry::Result set_rate_result = telemetry.set_rate_position(1.0);

    if (set_rate_result != Telemetry::Result::Success) {
        std::cerr << "Setting rate failed:" << set_rate_result << '\n';
        return;
    }

    // Set up callback to monitor altitude while the vehicle is in flight
    telemetry.subscribe_position([system](Telemetry::Position position) {
        int system_id = system->get_system_id();
        std::cout << "System ID: " << system_id << ", Altitude: " << position.relative_altitude_m << " m\n";
    });

    // Check if vehicle is ready to arm
    while (telemetry.health_all_ok() != true) {
        std::cout << "Vehicle is getting ready to arm\n";
        sleep_for(seconds(1));
    }

    // Arm vehicle
    std::cout << "Arming...\n";
    const Action::Result arm_result = action.arm();

    if (arm_result != Action::Result::Success) {
        std::cerr << "Arming failed:" << arm_result << '\n';
    }

    // Take off
    std::cout << "Taking off...\n";
    const Action::Result takeoff_result = action.takeoff();
    if (takeoff_result != Action::Result::Success) {
        std::cerr << "Takeoff failed:" << takeoff_result << '\n';
    }

    // Let it hover for a bit before landing again.
    sleep_for(seconds(20));

    std::cout << "Landing...\n";
    const Action::Result land_result = action.land();
    if (land_result != Action::Result::Success) {
        std::cerr << "Land failed:" << land_result << '\n';
    }

    // Check if vehicle is still in air
    while (telemetry.in_air()) {
        std::cout << "Vehicle is landing...\n";
        sleep_for(seconds(1));
    }
    std::cout << "Landed!\n";

    // We are relying on auto-disarming but let's keep watching the telemetry for a bit longer.

    sleep_for(seconds(5));
    std::cout << "Finished...\n";
    return;
}
