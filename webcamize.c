#include "libwebcamize/libwebcamize.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>

static struct webcamize_camera* g_cam = NULL;
static void signal_handler(int sig) {
    (void)sig;
    if (g_cam) webcamize_camera_stop(g_cam);
}

typedef enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_FATAL } LogLevel;
LogLevel log_level = LOG_LEVEL_INFO;
bool colors_enabled = true;

#define log(color, name, format, ...)                                                        \
    fprintf(stderr, "webcamize: %s [" name "] %s " format "\n", colors_enabled ? color : "", \
            colors_enabled ? "\e[0m" : "", ##__VA_ARGS__)
#define log_debug(format, ...) \
    if (log_level <= LOG_LEVEL_DEBUG) log("\e[0;106m", "DBUG", format, ##__VA_ARGS__)
#define log_info(format, ...) \
    if (log_level <= LOG_LEVEL_INFO) log("\e[0;102m", "INFO", format, ##__VA_ARGS__)
#define log_warn(format, ...) \
    if (log_level <= LOG_LEVEL_WARN) log("\e[0;105m", "WARN", format, ##__VA_ARGS__)
#define log_fatal(format, ...) log("\e[0;101m", "FATL", format, ##__VA_ARGS__)

void print_usage() {
    printf("\n");
    printf("Usage: webcamize [OPTIONS...]\n");
    printf("\n");
    printf("  -s,  --status                 Print a status report for webcamize and quit\n");
    printf(
        "  -c,  --camera NAME            Specify a camera to use by its name; autodetects by default\n");
    printf(
        "  -f,  --file [PATH]            Output to a file; if no argument is passed, output to stdout\n");
    printf("  -x,  --no-convert             Don't convert from input format before writing\n");
    printf(
        "  -p,  --fps VALUE              Specify the maximum frames per second (default: 60)\n");
    printf("\n");
    printf(
        "  -l,  --log-level LEVEL        Set the log level (DEBUG, INFO, WARN, FATAL; default: INFO)\n");
    printf("       --no-color               Disable the use of colors in the terminal\n");
    printf("  -v,  --version                Print version info and quit\n");
    printf("  -H,  --help                   Show this help message\n");
    printf("\n");
    printf(WEBCAMIZE_COPYRIGHT_LINE);
}

int print_status(struct webcamize_context* ctx) {
    printf(WEBCAMIZE_COPYRIGHT_LINE);
    printf("\n");
    printf("Libraries:\n");
    printf("   libgphoto2: %s\n", *gp_library_version(GP_VERSION_VERBOSE));
    printf("    libavutil: %d.%d.%d\n", LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR,
           LIBAVUTIL_VERSION_MICRO);
    printf("   libavcodec: %d.%d.%d\n", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR,
           LIBAVCODEC_VERSION_MICRO);
    printf("  libavformat: %d.%d.%d\n", LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR,
           LIBAVFORMAT_VERSION_MICRO);
    printf("   libswscale: %d.%d.%d\n", LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR,
           LIBSWSCALE_VERSION_MICRO);
    printf("\n");
    struct webcamize_camera_initializer status_cam_list[10];
    int count = 0;
    webcamize_result res
        = webcamize_context_cameras_enumerate(ctx, status_cam_list, 10, &count);
    if (res != WEBCAMIZE_RESULT_OK) {
        log_fatal("Failed to enumerate cameras: %s", webcamize_result_string(res));
        return res;
    }
    printf("Available cameras:\n");
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            printf(" %.2d) %s (%s)\n", i + 1, status_cam_list[i].name, status_cam_list[i].port);
        }
    } else {
        printf("  None detected\n");
    }
    printf("\n");
    return 0;
}

int main(int argc, char* argv[]) {
    bool do_status = false;
    bool no_convert = false;
    const char* camera_name = NULL;
    const char* output_path = NULL;
    bool output_to_file = false;
    int target_fps = 60;

    static struct option long_options[]
        = {{"status", no_argument, 0, 's'},     {"camera", required_argument, 0, 'c'},
           {"file", optional_argument, 0, 'f'}, {"no-convert", no_argument, 0, 'x'},
           {"fps", required_argument, 0, 'p'},  {"log-level", required_argument, 0, 'l'},
           {"no-color", no_argument, 0, 0},     {"version", no_argument, 0, 'v'},
           {"help", no_argument, 0, 'H'},       {0, 0, 0, 0}};

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "sc:f::xp:l:vH", long_options, &option_index))
           != -1) {
        switch (opt) {
            case 0:
                if (strcmp(long_options[option_index].name, "no-color") == 0)
                    colors_enabled = false;
                break;
            case 's':
                do_status = true;
                break;
            case 'c':
                camera_name = optarg;
                break;
            case 'f':
                output_to_file = true;
                output_path = optarg;
                break;
            case 'x':
                no_convert = true;
                break;
            case 'p':
                target_fps = atoi(optarg);
                if (target_fps <= 0) {
                    log_fatal("Invalid FPS value: %s", optarg);
                    return 1;
                }
                break;
            case 'l':
                if (strcmp(optarg, "DEBUG") == 0)
                    log_level = LOG_LEVEL_DEBUG;
                else if (strcmp(optarg, "INFO") == 0)
                    log_level = LOG_LEVEL_INFO;
                else if (strcmp(optarg, "WARN") == 0)
                    log_level = LOG_LEVEL_WARN;
                else if (strcmp(optarg, "FATAL") == 0)
                    log_level = LOG_LEVEL_FATAL;
                else {
                    log_fatal("Unknown log level: %s", optarg);
                    return 1;
                }
                break;
            case 'v':
                printf(WEBCAMIZE_COPYRIGHT_LINE);
                return 0;
            case 'H':
                print_usage();
                return 0;
            default:
                print_usage();
                return 1;
        }
    }

    struct webcamize_context ctx;
    webcamize_result res = webcamize_context_init(&ctx);
    if (res < WEBCAMIZE_RESULT_OK) {
        log_fatal("Failed to initialize main context: %s", webcamize_result_string(res));
        return res;
    }

    if (do_status) {
        int ret = print_status(&ctx);
        webcamize_context_cleanup(&ctx);
        return ret;
    }

    // Resolve camera
    struct webcamize_camera_initializer cam_init;
    if (camera_name) {
        int count = webcamize_context_camera_count(&ctx);
        bool found = false;
        struct webcamize_camera_initializer tmp;
        for (int i = 0; i < count; i++) {
            res = _webcamize_fill_initializer(&tmp, ctx._gp2_camlist, i);
            if (res < WEBCAMIZE_RESULT_OK) {
                log_fatal("Failed to read camera list: %s", webcamize_result_string(res));
                webcamize_context_cleanup(&ctx);
                return res;
            }
            if (strstr(tmp.name, camera_name) != NULL) {
                cam_init = tmp;
                found = true;
                break;
            }
        }
        if (!found) {
            log_fatal("Camera not found: %s", camera_name);
            webcamize_context_cleanup(&ctx);
            return 1;
        }
    } else {
        res = webcamize_context_camera_auto(&ctx, &cam_init);
        if (res < WEBCAMIZE_RESULT_OK) {
            log_fatal("No camera detected: %s", webcamize_result_string(res));
            webcamize_context_cleanup(&ctx);
            return res;
        }
    }

    log_info("Using camera: %s (%s)", cam_init.name, cam_init.port);

    // Open output
    int sink_fd;
    if (output_to_file && output_path) {
        sink_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink_fd < 0) {
            log_fatal("Failed to open output file '%s': %s", output_path, strerror(errno));
            webcamize_context_cleanup(&ctx);
            return 1;
        }
        log_info("Writing to file: %s", output_path);
    } else if (output_to_file) {
        sink_fd = STDOUT_FILENO;
        log_info("Writing to stdout");
    } else {
        log_fatal("No output specified. Use -f to output to a file or stdout.");
        print_usage();
        webcamize_context_cleanup(&ctx);
        return 1;
    }

    // Initialize camera
    struct webcamize_camera* cam = NULL;
    res = webcamize_camera_initialize(&ctx, &cam_init, &cam, sink_fd, target_fps, no_convert);
    if (res < WEBCAMIZE_RESULT_OK) {
        log_fatal("Failed to initialize camera: %s", webcamize_result_string(res));
        if (sink_fd != STDOUT_FILENO) close(sink_fd);
        webcamize_context_cleanup(&ctx);
        return res;
    }

    g_cam = cam;
    struct sigaction sa = {.sa_handler = signal_handler, .sa_flags = 0};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    log_info("Streaming at %dx%d, target %d fps", webcamize_camera_width(cam),
             webcamize_camera_height(cam), target_fps);

    res = webcamize_camera_loop(cam);
    if (res < WEBCAMIZE_RESULT_OK) {
        log_fatal("Camera loop error: %s", webcamize_result_string(res));
    }

    g_cam = NULL;

    webcamize_camera_cleanup(&cam);
    if (sink_fd != STDOUT_FILENO) close(sink_fd);
    webcamize_context_cleanup(&ctx);
    log_info("cleaning up!");
    return res;
}
