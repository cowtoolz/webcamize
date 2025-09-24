#define VERSION "2.0.2"
#define LICENSE "BSD-2-Clause"
#define AUTHOR "W. Turner Abney"
#define YEAR "2025"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-list.h>
#include <gphoto2/gphoto2-version.h>
#include <gphoto2/gphoto2-widget.h>
#include <gphoto2/gphoto2.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__WINDOWS__)
    #define OS_WINDOWS
#elif defined(__APPLE__) && defined(__MACH__)
    #define OS_MACOS
#elif defined(__linux__)
    #define OS_LINUX
#endif

typedef enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_FATAL } LogLevel;
LogLevel log_level = LOG_LEVEL_INFO;
bool colors_enabled = true;

int file_sink = -1;
int width = 640;
int height = 480;
long target_fps = 60;
bool no_convert = false;
char camera_model[32] = "";

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
#define COPYRIGHT_LINE "Webcamize " VERSION ", copyright (c) " AUTHOR " " YEAR ", licensed " LICENSE "\n"

#if defined(OS_LINUX)
    #include <linux/loop.h>
    #include <linux/module.h>
    #include <linux/videodev2.h>
    #include <sys/ioctl.h>
    #include <sys/stat.h>
    #include <sys/syscall.h>
    #include <sys/types.h>
    #include <sys/utsname.h>
    #include <sys/wait.h>

    #include <libkmod.h>

bool use_v4l2loopback = true;
bool v4l2_need_format_set = false;

struct v4l2_loopback_config {
    __s32 output_nr;
    __s32 unused;
    char card_label[32];
    __u32 min_width;
    __u32 max_width;
    __u32 min_height;
    __u32 max_height;
    __s32 max_buffers;
    __s32 max_openers;
    __s32 debug;
    __s32 announce_all_caps;
};

int v4l2_dev_num = -1;
int v4l2_fd = -1;
char v4l2_dev_path[20] = "\0";
int v4l2loopback_fd = -1;

int init_v4l2_device(void) {
    int ret;
    if (use_v4l2loopback) {
        v4l2loopback_fd = open("/dev/v4l2loopback", 0);
        if (v4l2loopback_fd < 0) {
            log_warn("Failed to open v4l2loopback control device, attempting to load kernel module: %s",
                     strerror(errno));
            struct kmod_ctx* kmod_context = kmod_new(NULL, NULL);
            struct kmod_module* mod;
            int ret = kmod_module_new_from_name(kmod_context, "v4l2loopback", &mod);
            if (ret != 0) {
                log_fatal("Failed to find v4l2loopback module: %s", strerror(errno));
                return -1;
            }

            ret = kmod_module_probe_insert_module(mod, KMOD_PROBE_IGNORE_LOADED, "devices=0 exclusive_caps=1", NULL,
                                                  NULL, NULL);
            if (ret < 0) {
                log_fatal("Failed to insert v4l2loopback module: %s", strerror(errno));
                return -1;
            } else {
                log_debug("The v4l2loopback module is present");
            }

            kmod_module_unref(mod);
            kmod_unref(kmod_context);

            // retry opening the v4l2loopback control device
            v4l2loopback_fd = open("/dev/v4l2loopback", 0);
            if (v4l2loopback_fd < 0) {
                log_warn("Failed to open v4l2loopback control device: %s", strerror(errno));
                return -1;
            }
        }

        struct v4l2_loopback_config cfg = {0};
        cfg.announce_all_caps = false;
        cfg.output_nr = (int32_t)v4l2_dev_num;

        if (strlen(camera_model) == 0) {
            snprintf(cfg.card_label, sizeof(cfg.card_label), "Webcamize");
        } else if (strlen(camera_model) > sizeof(cfg.card_label) - 11) {
            snprintf(cfg.card_label, sizeof(cfg.card_label), "%s", camera_model);
        } else {
            snprintf(cfg.card_label, sizeof(cfg.card_label), "%s Webcamize", camera_model);
        }

        ret = ioctl(v4l2loopback_fd, LOOP_CTL_ADD, &cfg);
        if (ret < 0) {
            log_warn("Failed to create a loopback device: %s", strerror(errno));
            log_warn("Falling back to an automatically selected device number");
            cfg.output_nr = -1;
            ret = ioctl(v4l2loopback_fd, LOOP_CTL_ADD, &cfg);
            if (ret < 0) {
                log_fatal("Failed to create a loopback device: %s", strerror(errno));
                return -1;
            }
        }
        v4l2_dev_num = ret;
    }
    sprintf(v4l2_dev_path, "/dev/video%d", v4l2_dev_num);
    log_debug("Initializing V4L2 device: %s", v4l2_dev_path);

    // Open the V4L2 device
    v4l2_fd = open(v4l2_dev_path, O_RDWR, O_NONBLOCK);
    if (v4l2_fd < 0) {
        log_warn("Failed to open V4L2 device %s: %s", v4l2_dev_path, strerror(errno));
        return -1;
    }

    // Check if this is a valid V4L2 device
    struct v4l2_capability cap;
    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        log_fatal("Device %s is not a valid V4L2 device: %s", v4l2_dev_path, strerror(errno));
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // Check if it supports video output
    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        log_fatal("Device %s does not support video output", v4l2_dev_path);
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    log_debug("V4L2 device initialized successfully");
    return 0;
}

int setup_v4l2_format(void) {
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = width * 2;  // YUYV = 2 bytes per pixel
    fmt.fmt.pix.sizeimage = width * height * 2;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_fatal("Could not set format for /dev/video%d: %s", v4l2_dev_num, strerror(errno));
        return -1;
    }

    log_debug("V4L2 format set to %dx%d YUYV", width, height);
    return 0;
}

int write_to_v4l2_device(const uint8_t* data, int data_size) {
    int n = write(v4l2_fd, data, data_size);
    if (n < 0) {
        log_fatal("Failed to write to V4L2 device: %s", strerror(errno));
        return -1;
    } else if (n != data_size) {
        log_warn("Short write to V4L2 device: wrote %d of %d bytes", n, data_size);
    }
    return 0;
}
#endif

AVCodecContext* decoder_ctx = NULL;
const AVCodec* decoder = NULL;
AVFrame* input_frame = NULL;
AVFrame* output_frame = NULL;
AVPacket* packet_obj = NULL;
struct SwsContext* sws_ctx = NULL;
uint8_t* ffmpeg_output_buffer = NULL;
int ffmpeg_output_buffer_size = 0;

int convert_ffmpeg(const char* image_data, unsigned long image_data_size, uint8_t** output_data, int* output_data_size);

volatile bool alive = true;
void sig_handler(int signo) {
    if (signo == SIGINT) alive = false;
}
int cli(int argc, char* argv[]);
void print_usage(void);
void print_status(void);

int main(int argc, char* argv[]) {
    int ret = cli(argc, argv);
    if (ret != 0) return ret;

    signal(SIGINT, sig_handler);

    Camera* gp2_camera = NULL;
    CameraFile* gp2_file = NULL;
    CameraList* gp2_camlist = NULL;
    GPContext* gp2_context = NULL;

#if defined(OS_LINUX)
    if (use_v4l2loopback && (geteuid() != 0)) {
        log_warn("Webcamize requires sudo when using v4l2loopback!");

        char executable_path[128];
        ret = readlink("/proc/self/exe", executable_path, sizeof(executable_path));
        if (ret == -1) {
            log_fatal("Failed to readlink own executable!");
            goto cleanup;
        }

        // Null-terminate the executable path
        if (ret >= (int)sizeof(executable_path)) {
            log_fatal("Executable path too long!");
            goto cleanup;
        }
        executable_path[ret] = '\0';

        pid_t pid = fork();
        if (pid == -1) {
            log_fatal("Failed to fork process!");
            goto cleanup;
        } else if (pid == 0) {
            // Child process: re-execute with sudo
            // Calculate new argv size: "sudo" + executable + original args + NULL
            int new_argc = argc + 2;
            char** new_argv = malloc(new_argc * sizeof(char*));
            if (!new_argv) {
                log_fatal("Failed to allocate memory for new argv!");
                exit(1);
            }

            // Build new argument list
            new_argv[0] = "sudo";
            new_argv[1] = executable_path;  // Full path to current executable

            // Copy original arguments (skip argv[0] since we use full path)
            for (int i = 1; i < argc; i++) {
                new_argv[i + 1] = argv[i];
            }
            new_argv[new_argc - 1] = NULL;

            // Execute sudo with the reconstructed arguments
            execvp("sudo", new_argv);

            // If we reach here, execvp failed
            log_fatal("Failed to execute sudo!");
            free(new_argv);
            exit(1);
        } else {
            // Parent process: wait for child to complete
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                // Child exited normally, exit with same code
                exit(WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                // Child was killed by signal
                log_fatal("Child process terminated by signal %d", WTERMSIG(status));
                exit(1);
            } else {
                // Unexpected termination
                log_fatal("Child process terminated unexpectedly");
                exit(1);
            }
        }
    }
#endif

    // Initialize gPhoto2 context
    gp2_context = gp_context_new();

    // Create camera object
    ret = gp_camera_new(&gp2_camera);
    if (ret < GP_OK) {
        log_fatal("Failed to instantiate a new camera: %s", gp_result_as_string(ret));
        goto cleanup;
    }

    gp_list_new(&gp2_camlist);
    ret = gp_camera_autodetect(gp2_camlist, gp2_context);
    if (ret < GP_OK) {
        log_fatal("Failed to autodetect cameras: %s", gp_result_as_string(ret));
        goto cleanup;
    }

    if (gp_list_count(gp2_camlist) < 1) {
        log_fatal("No cameras detected!");
        goto cleanup;
    }

    // If user specified a camera, check if it exists
    if (*camera_model) {
        int index = -1;
        ret = gp_list_find_by_name(gp2_camlist, &index, camera_model);
        if (ret < GP_OK) {
            log_warn("Camera '%s' not found, using first detected camera", camera_model);
            const char* first_camera = NULL;
            gp_list_get_name(gp2_camlist, 0, &first_camera);
            snprintf(camera_model, sizeof(camera_model), "%s", first_camera);
        } else {
            log_debug("Found requested camera: %s", camera_model);
            int gp2_camera_index = ret;

            static CameraAbilitiesList* gp2_abilities_list = NULL;
            ret = gp_abilities_list_new(&gp2_abilities_list);
            if (ret < GP_OK) {
                log_fatal("Failed to initialize abilities list: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            ret = gp_abilities_list_load(gp2_abilities_list, gp2_context);
            if (ret < GP_OK) {
                log_fatal("Failed to populate abilities list: %s", gp_result_as_string(ret));
                goto cleanup;
            }

            CameraAbilities gp2_abilities;
            ret = gp_abilities_list_lookup_model(gp2_abilities_list, camera_model);
            if (ret < GP_OK) {
                log_fatal("Lookup failed for specified model: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            ret = gp_abilities_list_get_abilities(gp2_abilities_list, ret, &gp2_abilities);
            if (ret < GP_OK) {
                log_fatal("Failed to get abilities for specified model: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            ret = gp_camera_set_abilities(gp2_camera, gp2_abilities);
            if (ret < GP_OK) {
                log_fatal("Failed to set abilities for specified model: %s", gp_result_as_string(ret));
                goto cleanup;
            }

            static GPPortInfoList* gp2_port_info_list = NULL;
            ret = gp_port_info_list_new(&gp2_port_info_list);
            if (ret < GP_OK) {
                log_fatal("Failed to initialize port info list: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            ret = gp_port_info_list_load(gp2_port_info_list);
            if (ret < GP_OK) {
                log_fatal("Failed to load port info list: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            ret = gp_port_info_list_count(gp2_port_info_list);
            if (ret < GP_OK) {
                log_fatal("Failed to populate count to port info list: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            const char* gp2_port_path;
            ret = gp_list_get_value(gp2_camlist, gp2_camera_index, &gp2_port_path);
            if (ret < GP_OK) {
                log_fatal("Failed to get port path for specified camera: %s", gp_result_as_string(ret));
                goto cleanup;
            }
            ret = gp_port_info_list_lookup_path(gp2_port_info_list, gp2_port_path);
            if (ret < GP_OK) {
                log_fatal("Lookup failed for the port of the specified camera within the port info list: %s",
                          gp_result_as_string(ret));
                goto cleanup;
            }
            int gp2_port_info_index = ret;
            GPPortInfo gp2_port_info;
            ret = gp_port_info_list_get_info(gp2_port_info_list, gp2_port_info_index, &gp2_port_info);
            if (ret < GP_OK) {
                log_fatal("Failed to get info for port from port info list: %s", gp_result_as_string(ret));
                goto cleanup;
            }

            ret = gp_camera_set_port_info(gp2_camera, gp2_port_info);
            if (ret < GP_OK) {
                log_fatal("Failed to set the port info of the camera to the specified port info: %s",
                          gp_result_as_string(ret));
                goto cleanup;
            }
        }
    } else {
        // No camera specified, use first detected
        const char* first_camera = NULL;
        gp_list_get_name(gp2_camlist, 0, &first_camera);
        snprintf(camera_model, sizeof(camera_model), "%s", first_camera);
    }

    log_debug("Using camera: %s", camera_model);

    ret = gp_camera_init(gp2_camera, gp2_context);
    if (ret < GP_OK) {
        log_fatal("Failed to autodetect camera: %s", gp_result_as_string(ret));
        goto cleanup;
    }

    ret = gp_file_new(&gp2_file);
    if (ret < GP_OK) {
        log_fatal("Failed to create CameraFile: %s", gp_result_as_string(ret));
        goto cleanup;
    }

#if defined(OS_LINUX)
    ret = init_v4l2_device();
    if (ret < 0) {
        log_fatal("Failed to initialize V4L2 device");
        goto cleanup;
    }

    if (*v4l2_dev_path) {
        v4l2_need_format_set = true;
        log_info("Starting webcam `%s` on %s!", camera_model, v4l2_dev_path);
    } else {
        log_info("Starting webcam `%s`!", camera_model);
    }
#else
    log_info("Starting webcam `%s`!", camera_model);
#endif

    if (!no_convert) {
        // Allocate frames
        input_frame = av_frame_alloc();
        if (!input_frame) {
            log_fatal("Failed to allocate input frame");
            goto cleanup;
        }

        output_frame = av_frame_alloc();
        if (!output_frame) {
            log_fatal("Failed to allocate output frame");
            goto cleanup;
        }
    }

    // main loop
    const char* image_data = NULL;
    unsigned long image_data_size;
    uint8_t* output_data = NULL;
    int output_data_size = 0;
    struct timespec frame_start = {};
    struct timespec frame_end = {};
    struct timespec sleep_time = {};
    long frame_time = 0;
    long target_frame_time = 1000000000L / target_fps;
    while (alive) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        ret = gp_camera_capture_preview(gp2_camera, gp2_file, gp2_context);
        if (ret != GP_OK) {
            log_fatal("Failed to capture preview: %s\n", gp_result_as_string(ret));
            break;
        }
        ret = gp_file_get_data_and_size(gp2_file, &image_data, &image_data_size);
        if (ret != GP_OK) {
            log_fatal("Failed to get data from camera file: %s\n", gp_result_as_string(ret));
            break;
        }

        if (!no_convert) {
            ret = convert_ffmpeg(image_data, image_data_size, &output_data, &output_data_size);
            if (ret < 0) {
                log_warn("Failed to convert image to YUYV, using original image data instead");
                output_data = (uint8_t*)image_data;
                output_data_size = image_data_size;
            }
        } else {
            output_data = (uint8_t*)image_data;
            output_data_size = image_data_size;
        }

        if (file_sink != -1) {
            int n = write(file_sink, output_data, output_data_size);
            if (n != output_data_size) {
                ret = errno;
                log_fatal("Failed to write all data to file sink, wrote %d of %d bytes: %s\n", n, output_data_size,
                          strerror(errno));
                break;
            }
            goto loop_end;
        }

#if defined(OS_LINUX)
        if (v4l2_fd > 0) {
            if (v4l2_need_format_set) {
                ret = setup_v4l2_format();
                if (ret < 0) {
                    log_fatal("Failed to set V4L2 format");
                    goto cleanup;
                }
                v4l2_need_format_set = false;
            }
            ret = write_to_v4l2_device(output_data, output_data_size);
            if (ret < 0) {
                log_fatal("Failed to write to V4L2 device");
                break;
            }
        }
#endif

    loop_end: {
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        frame_time = (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L + (frame_end.tv_nsec - frame_start.tv_nsec);
        if (frame_time < target_frame_time) {
            sleep_time.tv_nsec = target_frame_time - frame_time;
            nanosleep(&sleep_time, NULL);
        }
    }
    }

cleanup:
    // general
    if (file_sink) close(file_sink);

#if defined(OS_LINUX)
    // v4l2
    if (v4l2_fd > 0) {
        close(v4l2_fd);
        v4l2_fd = -1;
    }
    if (v4l2loopback_fd > 0) {
        ret = ioctl(v4l2loopback_fd, LOOP_CTL_REMOVE, v4l2_dev_num);
        if (ret < 0) {
            log_warn("Failed to remove the webcam device /dev/video%d: %s", v4l2_dev_num, strerror(errno));
            log_warn("Make sure no other programs are using the webcam before you close webcamize!");
        }
        close(v4l2loopback_fd);
        v4l2loopback_fd = -1;
    }
#endif

    // ffmpeg
    log_debug("Cleaning up ffmpeg...");
    if (ffmpeg_output_buffer) av_free(ffmpeg_output_buffer);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (output_frame) av_frame_free(&output_frame);
    if (input_frame) av_frame_free(&input_frame);
    if (decoder_ctx) avcodec_free_context(&decoder_ctx);
    if (packet_obj) av_packet_free(&packet_obj);

    // gphoto
    log_debug("Cleaning up gphoto2...");
    if (gp2_camlist) gp_list_free(gp2_camlist);
    if (gp2_file) gp_file_free(gp2_file);
    if (gp2_camera) {
        if (gp2_context) gp_camera_exit(gp2_camera, gp2_context);
        gp_camera_free(gp2_camera);
    }
    if (gp2_context) gp_context_unref(gp2_context);

    log_debug("Exiting, final ret = %d", ret);
    return ret < 0 ? 1 : 0;
}

int convert_ffmpeg(const char* image_data,
                   unsigned long image_data_size,
                   uint8_t** output_data,
                   int* output_data_size) {
    int ret;

    if (!decoder_ctx) {
        AVFormatContext* format_ctx = NULL;
        AVIOContext* avio_ctx = avio_alloc_context((unsigned char*)av_malloc(image_data_size), image_data_size, 0, NULL,
                                                   NULL, NULL, NULL);
        if (!avio_ctx) {
            log_warn("Failed to create AVIO context");
            return -1;
        }

        // Copy image data to the AVIO buffer
        memcpy(avio_ctx->buffer, image_data, image_data_size);

        // Allocate format context
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Failed to allocate format context");
            return -1;
        }

        // Set the AVIO context
        format_ctx->pb = avio_ctx;

        // Open input
        ret = avformat_open_input(&format_ctx, NULL, NULL, NULL);
        if (ret < 0) {
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avformat_free_context(format_ctx);
            log_warn("Failed to open input: %s", av_err2str(ret));
            return -1;
        }

        // Find stream info
        ret = avformat_find_stream_info(format_ctx, NULL);
        if (ret < 0) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Failed to find stream info: %s", av_err2str(ret));
            return -1;
        }

        // Find the first video stream
        int stream_index = -1;
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                stream_index = i;
                break;
            }
        }

        if (stream_index == -1) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("No video stream found");
            return -1;
        }

        // Get codec parameters
        AVCodecParameters* codec_params = format_ctx->streams[stream_index]->codecpar;

        // Find decoder
        decoder = avcodec_find_decoder(codec_params->codec_id);
        if (!decoder) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Decoder not found for codec ID: %d", codec_params->codec_id);
            return -1;
        }

        log_debug("Found decoder: %s for format: %s", decoder->name, format_ctx->iformat->name);

        // Create decoder context
        decoder_ctx = avcodec_alloc_context3(decoder);
        if (!decoder_ctx) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Could not allocate decoder context");
            return -1;
        }

        // Copy codec parameters to decoder context
        ret = avcodec_parameters_to_context(decoder_ctx, codec_params);
        if (ret < 0) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avcodec_free_context(&decoder_ctx);
            log_warn("Failed to copy codec parameters to decoder context: %s", av_err2str(ret));
            return -1;
        }

        // Various tweaks for low-latency decoding
        AVDictionary* opts = NULL;
        av_dict_set(&opts, "threads", "auto", 0);
        av_dict_set(&opts, "thread_type", "frame", 0);
        decoder_ctx->thread_count = 0;
        decoder_ctx->thread_type = FF_THREAD_FRAME;
        decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        decoder_ctx->get_buffer2 = avcodec_default_get_buffer2;
        decoder_ctx->lowres = 0;

        // Try to find a device for hardware acceleration
        enum AVHWDeviceType type = av_hwdevice_find_type_by_name("auto");
        if (type != AV_HWDEVICE_TYPE_NONE) {
            AVBufferRef* hw_device_ctx = NULL;
            ret = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0);
            if (ret >= 0) {
                decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                av_buffer_unref(&hw_device_ctx);
            }
        }

        // Open decoder
        ret = avcodec_open2(decoder_ctx, decoder, &opts);
        if (ret < 0) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avcodec_free_context(&decoder_ctx);
            log_warn("Could not open decoder: %s", av_err2str(ret));
            return -1;
        }

        // Set image dimensions from decoder context
        width = decoder_ctx->width;
        height = decoder_ctx->height;

        log_debug("Image dimensions: %dx%d", width, height);

        // Clean up format context (we only needed it for setup)
        avformat_close_input(&format_ctx);
        av_free(avio_ctx->buffer);
        avio_context_free(&avio_ctx);
    }

    if (!packet_obj) packet_obj = av_packet_alloc();

    packet_obj->data = (uint8_t*)image_data;
    packet_obj->size = image_data_size;

    // Send packet to decoder
    ret = avcodec_send_packet(decoder_ctx, packet_obj);
    if (ret < 0) {
        log_warn("Error sending packet to decoder: %s", av_err2str(ret));
        return -1;
    }

    // Receive frame from decoder
    ret = avcodec_receive_frame(decoder_ctx, input_frame);
    if (ret < 0) {
        log_warn("Error receiving frame from decoder: %s", av_err2str(ret));
        return -1;
    }

    // Initialize/update SwsContext if needed
    if (!sws_ctx || width != input_frame->width || height != input_frame->height) {
        width = input_frame->width;
        height = input_frame->height;

        if (sws_ctx) {
            sws_freeContext(sws_ctx);
        }

        sws_ctx = sws_getContext(width, height, input_frame->format, width, height, AV_PIX_FMT_YUYV422,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);

        if (!sws_ctx) {
            log_warn("Could not initialize SwsContext");
            return -1;
        }

        // Reallocate output buffer if needed
        int new_size = av_image_get_buffer_size(AV_PIX_FMT_YUYV422, width, height, 1);
        if (!ffmpeg_output_buffer || new_size > ffmpeg_output_buffer_size) {
            if (ffmpeg_output_buffer) {
                av_free(ffmpeg_output_buffer);
            }
            ffmpeg_output_buffer = (uint8_t*)av_malloc(new_size);
            if (!ffmpeg_output_buffer) {
                log_warn("Failed to reallocate FFmpeg output buffer");
                return -1;
            }
            ffmpeg_output_buffer_size = new_size;
        }

        // Set up output frame
        ret = av_image_fill_arrays(output_frame->data, output_frame->linesize, ffmpeg_output_buffer, AV_PIX_FMT_YUYV422,
                                   width, height, 1);
        if (ret < 0) {
            log_warn("Failed to set up output frame: %s", av_err2str(ret));
            return -1;
        }

        output_frame->width = width;
        output_frame->height = height;
        output_frame->format = AV_PIX_FMT_YUYV422;
    }

    // Rescale image
    ret = sws_scale(sws_ctx, (const uint8_t* const*)input_frame->data, input_frame->linesize, 0, height,
                    output_frame->data, output_frame->linesize);
    if (ret <= 0) {
        log_warn("Failed to convert image: %s", av_err2str(ret));
        return -1;
    }

    // Set output parameters
    *output_data = ffmpeg_output_buffer;
    *output_data_size = ffmpeg_output_buffer_size;

    return 0;
}

int cli(int argc, char* argv[]) {
    if (!isatty(STDERR_FILENO)) {
        colors_enabled = false;
    }

    static struct option long_options[] = {{"camera", required_argument, 0, 'c'},
                                           {"fps", required_argument, 0, 'p'},
                                           {"file", optional_argument, 0, 'f'},
                                           {"device", required_argument, 0, 'd'},
                                           {"log-level", required_argument, 0, 'l'},
                                           {"status", no_argument, 0, 's'},
                                           {"wait", no_argument, 0, 'w'},
                                           {"no-convert", no_argument, 0, 'x'},
                                           {"no-v4l2loopback", no_argument, 0, 'b'},
                                           {"no-color", no_argument, 0, 'o'},
                                           {"version", no_argument, 0, 'v'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};
    int option_index = 0;
    int c;
    // opterr = 0;
    while ((c = getopt_long(argc, argv, "ovxbc:f::wd:l:p:sh", long_options, &option_index)) != -1) {
        switch (c) {
            case 'v':
                log_info("Using webcamize %s", VERSION);
                return -1;

            case 'x':
                no_convert = true;
                break;

            case 'b':
#if defined(OS_LINUX)

                use_v4l2loopback = false;
#else
                log_warn("Option --no-v4l2loopback (-b) ignored as it does nothing on your operating system");
#endif
                break;

            case 'c':
                if (optarg) {
                    snprintf(camera_model, sizeof(camera_model), "%s", optarg);
                } else {
                    log_fatal("Missing argument for --camera (-c)");
                    return 1;
                }
                break;

            case 'f':
                if (optarg) {
                    file_sink = open(optarg, O_RDWR, O_NONBLOCK);
                    if (file_sink < 0) {
                        log_fatal("Failed to open file sink `%s`: %s", optarg, strerror(errno));
                        return 1;
                    }
                } else {
                    file_sink = STDOUT_FILENO;
                    log_info("Sink set to stdout because no argument was passed for option --file");
                }
                break;

            case 'p':
                if (optarg && !(!optarg || *optarg == '\0')) {
                    target_fps = atoi(optarg);
                    if (target_fps < 0) {
                        log_fatal("Argument for --fps (-p) must be a non-negative integer, got %s", optarg);
                        return 1;
                    }
                } else {
                    log_fatal("Missing argument for --fps (-p)");
                    return 1;
                }
                break;

            case 'd':
#if defined(OS_LINUX)
                if (optarg && !(!optarg || *optarg == '\0')) {
                    v4l2_dev_num = atoi(optarg);
                    if (v4l2_dev_num < 0) {
                        log_fatal("Argument for --device (-d) must be a non-negative integer, got %s", optarg);
                        return 1;
                    }
                } else {
                    log_fatal("Missing argument for --device (-d)");
                    return 1;
                }
#else
                log_warn("Option --device (-d) ignored as it does nothing on your operating system");
#endif
                break;

            case 'l':
                if (optarg) {
                    if (strcasecmp(optarg, "DEBUG") == 0) {
                        log_level = LOG_LEVEL_DEBUG;
                    } else if (strcasecmp(optarg, "INFO") == 0) {
                        log_level = LOG_LEVEL_INFO;
                    } else if (strcasecmp(optarg, "WARN") == 0) {
                        log_level = LOG_LEVEL_WARN;
                    } else if (strcasecmp(optarg, "FATAL") == 0) {
                        log_level = LOG_LEVEL_FATAL;
                    } else {
                        log_fatal("Invalid log level `%s`; must be one of DEBUG INFO WARN FATAL", optarg);
                        return 1;
                    }
                } else {
                    log_fatal("Missing argument for --log-level (-l)");
                    return 1;
                }
                break;

            case 's':
                print_status();
                return -1;

            case 'h':
                print_usage();
                return -1;

            case 'o':
                colors_enabled = false;
                break;

            case '?':
                // getopt_long already printed an error message
                print_usage();
                return 1;

            default:
                print_usage();
                log_fatal("Unsupported option %c", c);
                return 1;
        }
    }

    return 0;
}

void print_status() {
    printf("\n");
    printf(COPYRIGHT_LINE);
    printf("\n");
    printf("Libraries:\n");
    printf("   libgphoto2: %s\n", *(char**)gp_library_version(GP_VERSION_VERBOSE));
    printf("    libavutil: %d.%d.%d\n", LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, LIBAVUTIL_VERSION_MICRO);
    printf("   libavcodec: %d.%d.%d\n", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
    printf("  libavformat: %d.%d.%d\n", LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR,
           LIBAVFORMAT_VERSION_MICRO);
    printf("   libswscale: %d.%d.%d\n", LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO);
    printf("\n");
}

void print_usage() {
    printf("\n");
    printf("Usage: webcamize [OPTIONS...]\n");
    printf("\n");
    printf("  -s,  --status                 Print a status report for webcamize and quit\n");
    printf("  -c,  --camera NAME            Specify a camera to use by its name; autodetects by default\n");
    printf("  -f,  --file [PATH]            Output to a file; if no argument is passed, output to stdout\n");
    printf("  -x,  --no-convert             Don't convert from input format before writing\n");
    printf("  -p,  --fps VALUE              Specify the maximum frames per second (default: 60)\n");
#if defined(OS_LINUX)
    printf("  -d,  --device NUMBER          Specify the /dev/video_ device number to use\n");
    printf("  -b,  --no-v4l2loopback        Disable v4l2loopback module loading and configuration\n");
#endif
    printf("\n");
    printf("  -l,  --log-level LEVEL        Set the log level (DEBUG, INFO, WARN, FATAL; default: INFO)\n");
    printf("       --no-color               Disable the use of colors in the terminal\n");
    printf("  -v,  --version                Print version info and quit\n");
    printf("  -H,  --help                   Show this help message\n");
    printf("\n");
    printf(COPYRIGHT_LINE);
}
