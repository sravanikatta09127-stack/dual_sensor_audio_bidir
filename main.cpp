// general includes
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <tl/expected.hpp>
#include <cxxopts/cxxopts.hpp>

// GStreamer
#include <gst/gst.h>

// medialibrary includes
#include "hailo_analytics/pipeline/core/pipeline_builder.hpp"
#include "media_library/signal_utils.hpp"

// infra includes
#include "hailo_analytics/analytics/vision.hpp"
#include "hailo_analytics/logger/hailo_analytics_logger.hpp"

// ─────────────────────────────────────────────
#define APP_NAME        "dual_sensor_audio_bidir"
#define DEFAULT_HOST_IP "10.0.0.2"
#define NO_PROFILE_SELECTED ""
#define MEDIALIB_CONFIG_PATH_SENSOR_0 \
    "/etc/imaging/cfg/medialib_configs/case_studies/dual_sensor_single_stream_medialib_config_sensor_0.json"
#define MEDIALIB_CONFIG_PATH_SENSOR_1 \
    "/etc/imaging/cfg/medialib_configs/case_studies/dual_sensor_single_stream_medialib_config_sensor_1.json"

// Port map:
// 5000  sensor 0 video H264   -> host  (existing MediaLibrary pipeline)
// 5100  sensor 1 video H264   -> host  (existing MediaLibrary pipeline)
// 5200  audio L16 stereo      -> host  (NEW: board mic to host speakers)
// 5201  audio L16 stereo      -> board (NEW: host mic to board speaker)
#define PORT_AUDIO_SEND  5200
#define PORT_AUDIO_RECV  5201

// Audio hardware confirmed by testing:
// device=hw:0,0  (Hailo15-Audio-master, card 0 subdevice 0)
// capture format: S16LE, 48000Hz, 2ch (hardware requirement)
// send format:    S16BE, 48000Hz, 2ch (L16 RTP spec requirement)
// audioconvert handles the LE->BE swap transparently
#define AUDIO_DEVICE    "hw:0,0"
#define AUDIO_RATE      48000
#define AUDIO_CHANNELS  2

#undef PORT_FROM_ID
#define PORT_FROM_ID(sensor_idx, sink_id) \
    std::to_string(5000 + (sensor_idx)*100 + \
        std::stoi((sink_id).substr((sink_id).rfind("sink") + 4)) * 2)

// ─────────────────────────────────────────────
// Audio pipeline handles — file scope
// ─────────────────────────────────────────────
static GstElement *s_audio_send_pipeline = nullptr;
static GstElement *s_audio_recv_pipeline = nullptr;

// ─────────────────────────────────────────────
enum class ArgumentType {
    Help, PrintFPS, Timeout,
    ConfigSensor0, ConfigSensor1,
    ProfileSensor0, ProfileSensor1,
    HostIP, Error
};

void print_help(const cxxopts::Options &options) {
    std::cout << options.help() << std::endl;
}

cxxopts::Options build_arg_parser() {
    cxxopts::Options options(APP_NAME);
    options.add_options()
        ("h,help",    "Show this help")
        ("t,timeout", "Time to run",
            cxxopts::value<int>()->default_value("60"))
        ("p,print-fps", "Print FPS",
            cxxopts::value<bool>()->default_value("false"))
        ("c,config-file-path-sensor-0", "Medialib config sensor 0",
            cxxopts::value<std::string>()->default_value(MEDIALIB_CONFIG_PATH_SENSOR_0))
        ("d,config-file-path-sensor-1", "Medialib config sensor 1",
            cxxopts::value<std::string>()->default_value(MEDIALIB_CONFIG_PATH_SENSOR_1))
        ("a,profile-sensor-0", "Profile sensor 0",
            cxxopts::value<std::string>()->default_value(NO_PROFILE_SELECTED))
        ("b,profile-sensor-1", "Profile sensor 1",
            cxxopts::value<std::string>()->default_value(NO_PROFILE_SELECTED))
        ("o,host-ip", "Host IP for UDP output",
            cxxopts::value<std::string>()->default_value(DEFAULT_HOST_IP));
    return options;
}

std::vector<ArgumentType> handle_arguments(
    const cxxopts::ParseResult &result,
    const cxxopts::Options &options)
{
    std::vector<ArgumentType> arguments;
    if (result.count("help"))             { print_help(options); arguments.push_back(ArgumentType::Help); }
    if (result.count("print-fps"))        arguments.push_back(ArgumentType::PrintFPS);
    if (result.count("timeout"))          arguments.push_back(ArgumentType::Timeout);
    if (result.count("config-file-path-sensor-0")) arguments.push_back(ArgumentType::ConfigSensor0);
    if (result.count("config-file-path-sensor-1")) arguments.push_back(ArgumentType::ConfigSensor1);
    if (result.count("profile-sensor-0")) arguments.push_back(ArgumentType::ProfileSensor0);
    if (result.count("profile-sensor-1")) arguments.push_back(ArgumentType::ProfileSensor1);
    if (result.count("host-ip"))          arguments.push_back(ArgumentType::HostIP);
    for (const auto &u : result.unmatched()) {
        std::cerr << "Error: Unrecognized option: " << u << std::endl;
        return {ArgumentType::Error};
    }
    return arguments;
}

// ─────────────────────────────────────────────
struct MediaLibraryInstance {
    std::shared_ptr<MediaLibrary> media_library;
    std::string medialib_config_path;
    std::string profile_name;
};

struct AppResources {
    static constexpr int NUM_SENSORS = 2;
    std::vector<MediaLibraryInstance> instances;
    hailo_analytics::pipeline::PipelinePtr pipeline;
    bool print_fps = false;
    std::string host_ip = DEFAULT_HOST_IP;
    AppResources() { instances.resize(NUM_SENSORS); }
};

// ─────────────────────────────────────────────
std::string read_string_from_file(const char *file_path) {
    std::ifstream f(file_path);
    if (!f.is_open())
        throw std::runtime_error(std::string("config path not valid: ") + file_path);
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    std::cout << "Read config from file: " << file_path << std::endl;
    return s;
}

void configure_media_library(std::shared_ptr<AppResources> app_resources) {
    for (int i = 0; i < AppResources::NUM_SENSORS; ++i) {
        std::string cfg = read_string_from_file(
            app_resources->instances[i].medialib_config_path.c_str());
        auto ml = MediaLibrary::create();
        if (!ml.has_value())
            throw std::runtime_error("Failed to create media library for sensor "
                                     + std::to_string(i));
        app_resources->instances[i].media_library = ml.value();
        if (app_resources->instances[i].media_library->initialize(cfg)
                != media_library_return::MEDIA_LIBRARY_SUCCESS)
            throw std::runtime_error("Failed to initialize media library for sensor "
                                     + std::to_string(i));
        if (app_resources->instances[i].profile_name != NO_PROFILE_SELECTED)
            app_resources->instances[i].media_library->set_profile(
                app_resources->instances[i].profile_name);
    }
}

void create_pipeline(std::shared_ptr<AppResources> app_resources) {
    hailo_analytics::pipeline::PipelineBuilder pip_builder;
    for (int i = 0; i < AppResources::NUM_SENSORS; ++i) {
        auto output_streams =
            app_resources->instances[i].media_library->m_frontend->get_outputs_streams();
        if (!output_streams.has_value())
            throw std::runtime_error("Failed to get output streams for sensor "
                                     + std::to_string(i));

        auto custom_config =
            hailo_analytics::analytics::vision::base_vision_config(output_streams.value());

        for (auto &[stream_id, output_config] : custom_config.outputs) {
            std::string port = PORT_FROM_ID(i, stream_id);
            output_config.udp_config.host = app_resources->host_ip;
            output_config.udp_config.port = port;
            std::cout << "Video UDP: Sensor " << i
                      << " stream '" << stream_id << "' -> "
                      << app_resources->host_ip << ":" << port << std::endl;
        }

        auto sensor_pipeline = hailo_analytics::analytics::vision::generate_vision_pipeline(
            *app_resources->instances[i].media_library,
            "sensor_" + std::to_string(i) + "_pipeline",
            custom_config);
        if (!sensor_pipeline.has_value())
            throw std::runtime_error("Failed to create vision pipeline for sensor "
                                     + std::to_string(i));
        pip_builder.add_stage(sensor_pipeline.value());
    }
    app_resources->pipeline = pip_builder.build(APP_NAME, true);
}

// ─────────────────────────────────────────────
// Audio SEND pipeline
// board mic (hw:0,0) -> L16 RTP -> host:5200
//
// Pipeline explanation line by line:
//
// alsasrc device=hw:0,0
//   Captures from Hailo15-Audio-master hardware
//   Only format this device accepts: S16LE 48kHz stereo
//
// audio/x-raw,rate=48000,channels=2,format=S16LE
//   Explicit caps to force hardware to use this exact format
//   Prevents GStreamer from negotiating something the device rejects
//
// audioconvert
//   Converts S16LE (little-endian) to S16BE (big-endian)
//   L16 RTP standard (RFC 3551) requires big-endian samples
//   audioconvert handles this transparently — no quality loss
//
// audio/x-raw,rate=48000,channels=2,format=S16BE
//   Forces the output of audioconvert to big-endian
//
// rtpL16pay pt=96
//   Packetizes raw PCM as RTP L16 payload type 96
//   L16 = uncompressed 16-bit linear PCM
//   No codec, no encoder, no experimental issues
//   Bandwidth: 48000 * 2 * 2 = 192000 bytes/s = 1.5 Mbps
//   Acceptable on local Ethernet, not for WAN
//
// udpsink host=X port=5200
//   Sends RTP packets to host machine
// ─────────────────────────────────────────────
static void start_audio_send(const std::string &host_ip)
{
    std::string pipeline_str =
        "alsasrc device=" AUDIO_DEVICE " ! "
        "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
        "audioconvert ! "
        "audio/x-raw,rate=48000,channels=2,format=S16BE ! "
        "rtpL16pay pt=96 ! "
        "udpsink host=" + host_ip + " port=" + std::to_string(PORT_AUDIO_SEND);

    std::cout << "Audio send: " AUDIO_DEVICE
              << " -> L16 RTP stereo 48kHz -> "
              << host_ip << ":" << PORT_AUDIO_SEND << std::endl;

    GError *err = nullptr;
    s_audio_send_pipeline = gst_parse_launch(pipeline_str.c_str(), &err);
    if (!s_audio_send_pipeline || err) {
        std::cerr << "FATAL: audio send pipeline failed: "
                  << (err ? err->message : "unknown") << std::endl;
        if (err) g_error_free(err);
        std::exit(1);
    }
    if (gst_element_set_state(s_audio_send_pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "FATAL: audio send pipeline failed to start" << std::endl;
        gst_object_unref(s_audio_send_pipeline);
        s_audio_send_pipeline = nullptr;
        std::exit(1);
    }
    std::cout << "Audio send pipeline running." << std::endl;
}

// ─────────────────────────────────────────────
// Audio RECEIVE pipeline
// host:5201 -> L16 RTP -> board speaker (hw:0,0)
//
// Pipeline explanation line by line:
//
// udpsrc port=5201
//   Listens for incoming RTP packets from host
//
// caps=application/x-rtp,...
//   Tells udpsrc what format to expect
//   Must match what the host sends (L16, 48kHz, stereo, pt=96)
//
// rtpjitterbuffer latency=200
//   Buffers 200ms of audio packets
//   Smooths out network jitter — prevents audio glitches
//   200ms is good for LAN, increase to 500ms for WAN
//
// rtpL16depay
//   Strips RTP header, outputs raw S16BE PCM
//
// audioconvert
//   Converts S16BE back to S16LE for the hardware
//
// audio/x-raw,rate=48000,channels=2,format=S16LE
//   Forces output format the hardware accepts
//
// alsasink device=hw:0,0 sync=false
//   Plays audio through Hailo15-Audio-master
//   sync=false: do not wait for clock sync — play as fast as data arrives
//   This prevents buffer starvation when network timing varies
// ─────────────────────────────────────────────
static void start_audio_recv()
{
    std::string pipeline_str =
        "udpsrc port=" + std::to_string(PORT_AUDIO_RECV) + " "
        "caps=\"application/x-rtp,media=audio,clock-rate=48000,"
        "encoding-name=L16,channels=2,payload=96\" ! "
        "rtpjitterbuffer latency=200 ! "
        "rtpL16depay ! "
        "audioconvert ! "
        "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
        "alsasink device=" AUDIO_DEVICE " sync=false";

    std::cout << "Audio recv: UDP :" << PORT_AUDIO_RECV
              << " -> L16 decode -> " AUDIO_DEVICE << std::endl;

    GError *err = nullptr;
    s_audio_recv_pipeline = gst_parse_launch(pipeline_str.c_str(), &err);
    if (!s_audio_recv_pipeline || err) {
        std::cerr << "FATAL: audio recv pipeline failed: "
                  << (err ? err->message : "unknown") << std::endl;
        if (err) g_error_free(err);
        std::exit(1);
    }
    if (gst_element_set_state(s_audio_recv_pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "FATAL: audio recv pipeline failed to start" << std::endl;
        gst_object_unref(s_audio_recv_pipeline);
        s_audio_recv_pipeline = nullptr;
        std::exit(1);
    }
    std::cout << "Audio recv pipeline running." << std::endl;
}

// ─────────────────────────────────────────────
static void stop_audio_pipelines()
{
    if (s_audio_send_pipeline) {
        gst_element_set_state(s_audio_send_pipeline, GST_STATE_NULL);
        gst_object_unref(s_audio_send_pipeline);
        s_audio_send_pipeline = nullptr;
    }
    if (s_audio_recv_pipeline) {
        gst_element_set_state(s_audio_recv_pipeline, GST_STATE_NULL);
        gst_object_unref(s_audio_recv_pipeline);
        s_audio_recv_pipeline = nullptr;
    }
    std::cout << "Audio pipelines stopped." << std::endl;
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
std::mutex              g_stop_mutex;
std::condition_variable g_stop_cv;

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    auto app_resources = std::make_shared<AppResources>();
    app_resources->instances[0].medialib_config_path = MEDIALIB_CONFIG_PATH_SENSOR_0;
    app_resources->instances[1].medialib_config_path = MEDIALIB_CONFIG_PATH_SENSOR_1;

    signal_utils::SignalHandler signal_handler(false);
    signal_handler.register_signal_handler([]([[maybe_unused]] int signal) {
        std::cout << "Stopping..." << std::endl;
        g_stop_cv.notify_all();
    });

    cxxopts::Options options = build_arg_parser();
    auto result = options.parse(argc, argv);
    auto argument_handling_results = handle_arguments(result, options);
    int timeout = result["timeout"].as<int>();

    for (auto argument : argument_handling_results) {
        switch (argument) {
        case ArgumentType::Help:         return 0;
        case ArgumentType::Timeout:      break;
        case ArgumentType::PrintFPS:     app_resources->print_fps = true; break;
        case ArgumentType::ConfigSensor0:
            app_resources->instances[0].medialib_config_path =
                result["config-file-path-sensor-0"].as<std::string>(); break;
        case ArgumentType::ConfigSensor1:
            app_resources->instances[1].medialib_config_path =
                result["config-file-path-sensor-1"].as<std::string>(); break;
        case ArgumentType::ProfileSensor0:
            app_resources->instances[0].profile_name =
                result["profile-sensor-0"].as<std::string>(); break;
        case ArgumentType::ProfileSensor1:
            app_resources->instances[1].profile_name =
                result["profile-sensor-1"].as<std::string>(); break;
        case ArgumentType::HostIP:
            app_resources->host_ip = result["host-ip"].as<std::string>(); break;
        case ArgumentType::Error:        return 1;
        }
    }

    // Step 1: configure MediaLibrary for both sensors
    configure_media_library(app_resources);

    // Step 2: build Hailo video pipeline
    create_pipeline(app_resources);

    // Step 3: start audio pipelines
    // Audio send starts first — device opens immediately
    // Audio recv starts second — just listens, no device to open
    start_audio_send(app_resources->host_ip);
    start_audio_recv();

    // Step 4: start video pipeline
    // Started after audio to ensure audio is ready when video begins
    std::cout << "Starting video pipeline." << std::endl;
    app_resources->pipeline->start();

    std::cout << "\n=== Running ===" << std::endl;
    std::cout << "Video sensor 0 -> " << app_resources->host_ip << ":5000" << std::endl;
    std::cout << "Video sensor 1 -> " << app_resources->host_ip << ":5100" << std::endl;
    std::cout << "Audio send     -> " << app_resources->host_ip << ":" << PORT_AUDIO_SEND << std::endl;
    std::cout << "Audio recv     <- :" << PORT_AUDIO_RECV << std::endl;
    std::cout << "Running for " << timeout << "s (Ctrl+C to stop)" << std::endl;

    std::unique_lock<std::mutex> lk(g_stop_mutex);
    g_stop_cv.wait_for(lk, std::chrono::seconds(timeout));

    std::cout << "Stopping." << std::endl;
    app_resources->pipeline->stop();
    stop_audio_pipelines();
    return 0;
}
