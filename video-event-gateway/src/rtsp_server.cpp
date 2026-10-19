#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib-unix.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

gboolean stop_loop(gpointer context) {
    g_main_loop_quit(static_cast<GMainLoop*>(context));
    return G_SOURCE_REMOVE;
}

std::string file_pipeline(const std::string& file) {
    gchar* escaped = g_strescape(file.c_str(), nullptr);
    if (!escaped) throw std::runtime_error("could not escape file path");
    const std::string pipeline =
        "( filesrc location=\"" + std::string(escaped) +
        "\" ! decodebin ! videoconvert ! video/x-raw,format=I420"
        " ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=600 key-int-max=20"
        " ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )";
    g_free(escaped);
    return pipeline;
}

void print_usage() {
    std::cout << "Usage: veg_rtsp_server [--file fixture.avi | --test-pattern]"
                 " [--port 8554] [--mount /crossing]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string file;
        std::string port = "8554";
        std::string mount = "/crossing";
        bool test_pattern = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                print_usage();
                return 0;
            }
            if ((arg == "--file" || arg == "--port" || arg == "--mount") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--file") file = value;
                else if (arg == "--port") port = value;
                else mount = value;
            } else if (arg == "--test-pattern") {
                test_pattern = true;
            } else {
                print_usage();
                throw std::runtime_error("unknown or incomplete argument: " + arg);
            }
        }
        if (file.empty() == !test_pattern) {
            throw std::runtime_error("choose exactly one of --file or --test-pattern");
        }
        if (mount.empty() || mount.front() != '/' || mount.find(' ') != std::string::npos) {
            throw std::runtime_error("mount must be an absolute path without spaces");
        }
        const int port_number = std::stoi(port);
        if (port_number < 1 || port_number > 65535) {
            throw std::runtime_error("port must be between 1 and 65535");
        }
        if (!file.empty()) {
            file = std::filesystem::absolute(file).string();
            if (!std::filesystem::is_regular_file(file)) {
                throw std::runtime_error("video file does not exist: " + file);
            }
        }

        gst_init(nullptr, nullptr);
        const std::string pipeline = test_pattern
            ? "( videotestsrc is-live=true pattern=ball"
              " ! video/x-raw,width=640,height=360,framerate=20/1"
              " ! videoconvert ! video/x-raw,format=I420"
              " ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=600 key-int-max=20"
              " ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )"
            : file_pipeline(file);

        GstRTSPServer* server = gst_rtsp_server_new();
        gst_rtsp_server_set_service(server, port.c_str());
        GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, pipeline.c_str());
        gst_rtsp_media_factory_set_shared(factory, FALSE);
        gst_rtsp_mount_points_add_factory(mounts, mount.c_str(), factory);
        g_object_unref(mounts);

        GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
        const guint source_id = gst_rtsp_server_attach(server, nullptr);
        if (source_id == 0) {
            g_main_loop_unref(loop);
            g_object_unref(server);
            throw std::runtime_error("could not listen on RTSP port " + port);
        }
        g_unix_signal_add(SIGINT, stop_loop, loop);
        g_unix_signal_add(SIGTERM, stop_loop, loop);
        std::cout << "RTSP ready: rtsp://127.0.0.1:" << port << mount << std::endl;
        g_main_loop_run(loop);

        g_source_remove(source_id);
        g_main_loop_unref(loop);
        g_object_unref(server);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "veg_rtsp_server: " << error.what() << '\n';
        return 1;
    }
}
