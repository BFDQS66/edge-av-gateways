#include "veg/core.hpp"

#include <opencv2/core.hpp>

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        veg::Config config;
        veg::EventTracker tracker(config);
        int line_events = 0;
        int zone_events = 0;
        const std::vector<int> positions{100, 150, 200, 250, 300, 330, 370, 410, 450, 490};
        for (std::size_t frame = 0; frame < positions.size(); ++frame) {
            const auto events = tracker.update({{positions[frame], 140, 36, 56}},
                                               static_cast<std::int64_t>(frame),
                                               frame * 50.0, 640, 360);
            for (const auto& event : events) {
                if (event.type == veg::EventType::line_cross) ++line_events;
                if (event.type == veg::EventType::zone_enter) ++zone_events;
            }
        }
        require(line_events == 1, "crossing should produce one line event");
        require(zone_events == 1, "entry should produce one zone event");

        veg::Config dwell_config;
        dwell_config.dwell_frames = 4;
        veg::EventTracker dwell_tracker(dwell_config);
        int dwell_events = 0;
        for (int frame = 0; frame < 12; ++frame) {
            const auto events = dwell_tracker.update({{390, 140, 36, 56}},
                                                     frame, frame * 50.0, 640, 360);
            for (const auto& event : events) {
                if (event.type == veg::EventType::dwell) ++dwell_events;
            }
        }
        require(dwell_events == 1, "staying in the zone should emit one dwell event");

        veg::EventTracker reverse(config);
        for (int frame = 0; frame < 8; ++frame) {
            const auto events = reverse.update({{500 - frame * 40, 140, 36, 56}},
                                               frame, frame * 50.0, 640, 360);
            for (const auto& event : events) {
                require(event.type != veg::EventType::line_cross,
                        "right-to-left motion should not fire left-to-right line rule");
            }
        }
        std::cout << "rule tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
