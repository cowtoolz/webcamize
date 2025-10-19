#pragma once

#define WEBCAMIZE_VERSION "3.0.0"
#define WEBCAMIZE_AUTHOR "W. Turner Abney"
#define WEBCAMIZE_LICENSE "BSD-2-Clause"
#define WEBCAMIZE_YEAR "2026"
#define WEBCAMIZE_COPYRIGHT_LINE                                                          \
    "Webcamize " WEBCAMIZE_VERSION ", copyright (c) " WEBCAMIZE_AUTHOR " " WEBCAMIZE_YEAR \
    ", licensed " WEBCAMIZE_LICENSE "\n"

#include <stdatomic.h>
#include <stdbool.h>
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

typedef int webcamize_result;

static const int _WEBCAMIZE_LIBC_RES_OFFSET = -30000;
static inline webcamize_result _webcamize_result_from_errno(int eno) {
    if (eno >= 0) return eno;
    return _WEBCAMIZE_LIBC_RES_OFFSET + eno;
};

static const int _WEBCAMIZE_GP2_RES_OFFSET = -20000;
static inline webcamize_result _webcamize_result_from_gp2_result(int gp2_res) {
    if (gp2_res >= GP_OK) return gp2_res;
    return _WEBCAMIZE_GP2_RES_OFFSET + gp2_res;
};

static const int _WEBCAMIZE_AVU_RES_OFFSET = -10000;
static inline webcamize_result _webcamize_result_from_avu_result(int avu_res) {
    if (avu_res >= 0) return avu_res;
    return _WEBCAMIZE_AVU_RES_OFFSET + avu_res;
};

#define WEBCAMIZE_RESULT_OK 0
#define WEBCAMIZE_RESULT_CAMERA_NOT_INITIALIZED -1
#define WEBCAMIZE_RESULT_CAM_NULL_FRAMES -2
#define WEBCAMIZE_RESULT_NULL_AVIO_CTX -3
#define WEBCAMIZE_RESULT_NULL_AVFORMAT_CTX -4
#define WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER -5
#define WEBCAMIZE_RESULT_NO_STREAM -6
#define WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER_CTX -7
#define WEBCAMIZE_RESULT_CAM_NULL_SWSCALE_CTX -8
#define WEBCAMIZE_RESULT_CAM_NULL_SWS_OUTPUT_BUF -9
#define WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_PACKET -10

static inline const char* webcamize_result_string(webcamize_result res) {
    if (res >= WEBCAMIZE_RESULT_OK) return "OK";
    static _Thread_local char str[256];
    if (res <= _WEBCAMIZE_LIBC_RES_OFFSET) {
        snprintf(str, sizeof(str), "encountered an error: %s",
                 strerror(res - _WEBCAMIZE_LIBC_RES_OFFSET));
    } else if (res <= _WEBCAMIZE_GP2_RES_OFFSET) {
        snprintf(str, sizeof(str), "encountered gphoto2 error: %s",
                 gp_result_as_string(res - _WEBCAMIZE_GP2_RES_OFFSET));
    } else if (res <= _WEBCAMIZE_AVU_RES_OFFSET) {
        snprintf(str, sizeof(str), "encountered ffmpeg error: %s",
                 av_err2str(res - _WEBCAMIZE_AVU_RES_OFFSET));
    } else {
        switch (res) {
            case WEBCAMIZE_RESULT_CAMERA_NOT_INITIALIZED:
                return "camera not initialized";
            case WEBCAMIZE_RESULT_CAM_NULL_FRAMES:
                return "camera: input or output frame was null";
            case WEBCAMIZE_RESULT_NULL_AVIO_CTX:
                return "AVIO context was null";
            case WEBCAMIZE_RESULT_NULL_AVFORMAT_CTX:
                return "avformat context was null";
            case WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER:
                return "camera: avcodec decoder was null";
            case WEBCAMIZE_RESULT_NO_STREAM:
                return "no suitable stream found";
            case WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER_CTX:
                return "camera: avcodec decoder context was null";
            case WEBCAMIZE_RESULT_CAM_NULL_SWSCALE_CTX:
                return "camera: swscale context was null";
            case WEBCAMIZE_RESULT_CAM_NULL_SWS_OUTPUT_BUF:
                return "camera: swscale output buffer was null";
            case WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_PACKET:
                return "camera: avcodec packet was null";
            default:
                return "unknown error!";
        }
    }
    return str;
};

struct webcamize_context {
    GPContext* _gp2_context;
    CameraList* _gp2_camlist;
};

struct webcamize_camera_initializer {
    char name[128];
    char port[128];
    int index;
};

struct webcamize_camera {
    bool _initialized;
    atomic_bool _stop;
    int _width;
    int _height;
    int _sink_fd;
    int _target_fps;
    bool _no_convert;
    const char* image_data;
    unsigned long image_data_size;
    uint8_t* output_data;
    int output_data_size;
    Camera* _gp2_camera;
    CameraFile* _gp2_file;
    GPContext* _gp2_ctx;
    AVCodecContext* _avc_decoder_ctx;
    const AVCodec* _avc_decoder;
    AVPacket* _avc_packet;
    AVFrame* _avu_input_frame;
    AVFrame* _avu_output_frame;
    SwsContext* _sws_ctx;
    uint8_t* _sws_output_buffer;
    int _sws_output_buffer_size;
};

static inline webcamize_result webcamize_context_init(struct webcamize_context* ctx) {
    memset(ctx, 0, sizeof(struct webcamize_context));

    GPContext* gp2_context = gp_context_new();
    CameraList* gp2_camlist = NULL;
    int gp2_ret = gp_list_new(&gp2_camlist);
    if (gp2_ret < GP_OK) {
        gp_context_unref(gp2_context);
        return _webcamize_result_from_gp2_result(gp2_ret);
    }
    gp2_ret = gp_camera_autodetect(gp2_camlist, gp2_context);
    if (gp2_ret < GP_OK) {
        gp_list_free(gp2_camlist);
        gp_context_unref(gp2_context);
        return _webcamize_result_from_gp2_result(gp2_ret);
    }
    ctx->_gp2_context = gp2_context;
    ctx->_gp2_camlist = gp2_camlist;
    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result webcamize_context_cleanup(struct webcamize_context* ctx) {
    if (!ctx) return WEBCAMIZE_RESULT_OK;

    int gp2_ret = 0;
    if (ctx->_gp2_camlist) gp2_ret = gp_list_free(ctx->_gp2_camlist);
    if (ctx->_gp2_context) gp_context_unref(ctx->_gp2_context);
    memset(ctx, 0, sizeof(struct webcamize_context));
    return _webcamize_result_from_gp2_result(gp2_ret);
}

static inline webcamize_result _webcamize_fill_initializer(
    struct webcamize_camera_initializer* init,
    CameraList* camlist,
    int index) {
    const char* name;
    const char* port;
    int ret = gp_list_get_name(camlist, index, &name);
    if (ret < GP_OK) return _webcamize_result_from_gp2_result(ret);
    ret = gp_list_get_value(camlist, index, &port);
    if (ret < GP_OK) return _webcamize_result_from_gp2_result(ret);
    strncpy(init->name, name, sizeof(init->name) - 1);
    init->name[sizeof(init->name) - 1] = '\0';
    strncpy(init->port, port, sizeof(init->port) - 1);
    init->port[sizeof(init->port) - 1] = '\0';
    init->index = index;
    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result webcamize_context_cameras_enumerate(
    struct webcamize_context* ctx,
    struct webcamize_camera_initializer* cam_init_list,
    int capacity,
    int* count) {
    int num_cameras = gp_list_count(ctx->_gp2_camlist);
    if (num_cameras < 0) return _webcamize_result_from_gp2_result(num_cameras);
    if (num_cameras > capacity) num_cameras = capacity;
    for (int i = 0; i < num_cameras; i++) {
        webcamize_result res
            = _webcamize_fill_initializer(&cam_init_list[i], ctx->_gp2_camlist, i);
        if (res < WEBCAMIZE_RESULT_OK) return res;
    }
    *count = num_cameras;
    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result webcamize_context_camera_auto(
    struct webcamize_context* ctx,
    struct webcamize_camera_initializer* init) {
    if (gp_list_count(ctx->_gp2_camlist) < 1) return WEBCAMIZE_RESULT_CAMERA_NOT_INITIALIZED;
    return _webcamize_fill_initializer(init, ctx->_gp2_camlist, 0);
}

static inline webcamize_result _webcamize_setup_gp2_camera(
    struct webcamize_context* ctx,
    struct webcamize_camera* cam,
    struct webcamize_camera_initializer* cam_init) {
    CameraAbilitiesList* gp2_abilities_list = NULL;
    GPPortInfoList* gp2_port_info_list = NULL;
    webcamize_result result = WEBCAMIZE_RESULT_OK;
    CameraAbilities gp2_abilities;
    const char* gp2_port_path;
    int gp2_port_info_index;
    GPPortInfo gp2_port_info;

    int ret = gp_abilities_list_new(&gp2_abilities_list);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_abilities_list_load(gp2_abilities_list, ctx->_gp2_context);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_abilities_list_lookup_model(gp2_abilities_list, cam_init->name);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_abilities_list_get_abilities(gp2_abilities_list, ret, &gp2_abilities);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_camera_set_abilities(cam->_gp2_camera, gp2_abilities);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_port_info_list_new(&gp2_port_info_list);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_port_info_list_load(gp2_port_info_list);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_port_info_list_count(gp2_port_info_list);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_list_get_value(ctx->_gp2_camlist, cam_init->index, &gp2_port_path);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_port_info_list_lookup_path(gp2_port_info_list, gp2_port_path);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    gp2_port_info_index = ret;
    ret = gp_port_info_list_get_info(gp2_port_info_list, gp2_port_info_index, &gp2_port_info);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

    ret = gp_camera_set_port_info(cam->_gp2_camera, gp2_port_info);
    if (ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(ret);
        goto cleanup;
    }

cleanup:
    if (gp2_port_info_list) gp_port_info_list_free(gp2_port_info_list);
    if (gp2_abilities_list) gp_abilities_list_free(gp2_abilities_list);
    return result;
}

static inline int webcamize_context_camera_count(struct webcamize_context* ctx) {
    return gp_list_count(ctx->_gp2_camlist);
}

static inline webcamize_result _webcamize_setup_swscale(struct webcamize_camera* cam,
                                                        enum AVPixelFormat src_fmt) {
    if (cam->_sws_ctx) sws_freeContext(cam->_sws_ctx);

    cam->_sws_ctx
        = sws_getContext(cam->_width, cam->_height, src_fmt, cam->_width, cam->_height,
                         AV_PIX_FMT_YUYV422, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!cam->_sws_ctx) return WEBCAMIZE_RESULT_CAM_NULL_SWSCALE_CTX;

    int new_size = av_image_get_buffer_size(AV_PIX_FMT_YUYV422, cam->_width, cam->_height, 1);
    if (new_size > cam->_sws_output_buffer_size) {
        if (cam->_sws_output_buffer) av_free(cam->_sws_output_buffer);
        cam->_sws_output_buffer = (uint8_t*)av_malloc(new_size);
        if (!cam->_sws_output_buffer) return WEBCAMIZE_RESULT_CAM_NULL_SWS_OUTPUT_BUF;
        cam->_sws_output_buffer_size = new_size;
    }

    int ret = av_image_fill_arrays(cam->_avu_output_frame->data,
                                   cam->_avu_output_frame->linesize, cam->_sws_output_buffer,
                                   AV_PIX_FMT_YUYV422, cam->_width, cam->_height, 1);
    if (ret < 0) return _webcamize_result_from_avu_result(ret);

    cam->_avu_output_frame->width = cam->_width;
    cam->_avu_output_frame->height = cam->_height;
    cam->_avu_output_frame->format = AV_PIX_FMT_YUYV422;
    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result _webcamize_camera_setup_ffmpeg(struct webcamize_camera* cam) {
    if (cam->_no_convert) return WEBCAMIZE_RESULT_OK;

    webcamize_result result = WEBCAMIZE_RESULT_OK;
    unsigned char* avio_buf;
    AVIOContext* avio_ctx = NULL;
    AVFormatContext* format_ctx = NULL;
    AVDictionary* opts = NULL;
    AVCodecParameters* codec_params = NULL;
    int stream_index;
    int ret;

    cam->_avu_input_frame = av_frame_alloc();
    cam->_avu_output_frame = av_frame_alloc();
    if (!cam->_avu_input_frame || !cam->_avu_output_frame) {
        result = WEBCAMIZE_RESULT_CAM_NULL_FRAMES;
        goto error;
    }

    avio_buf = (unsigned char*)av_malloc(cam->image_data_size);
    if (!avio_buf) {
        result = WEBCAMIZE_RESULT_NULL_AVIO_CTX;
        goto error;
    }
    avio_ctx = avio_alloc_context(avio_buf, cam->image_data_size, 0, NULL, NULL, NULL, NULL);
    if (!avio_ctx) {
        av_free(avio_buf);
        result = WEBCAMIZE_RESULT_NULL_AVIO_CTX;
        goto error;
    }

    memcpy(avio_ctx->buffer, cam->image_data, cam->image_data_size);

    format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        result = WEBCAMIZE_RESULT_NULL_AVFORMAT_CTX;
        goto probe_cleanup;
    }

    format_ctx->pb = avio_ctx;

    ret = avformat_open_input(&format_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        format_ctx = NULL;
        result = _webcamize_result_from_avu_result(ret);
        goto probe_cleanup;
    }

    ret = avformat_find_stream_info(format_ctx, NULL);
    if (ret < 0) {
        result = _webcamize_result_from_avu_result(ret);
        goto probe_cleanup;
    }

    stream_index = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_index = i;
            break;
        }
    }

    if (stream_index == -1) {
        result = WEBCAMIZE_RESULT_NO_STREAM;
        goto probe_cleanup;
    }

    codec_params = format_ctx->streams[stream_index]->codecpar;

    // if (codec_params->codec_id == AV_CODEC_ID_MJPEG) {
    //     cam->_no_convert = true;

    //     // Decode one frame just to get dimensions
    //     const AVCodec* mjpeg_dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    //     if (mjpeg_dec) {
    //         AVCodecContext* tmp_ctx = avcodec_alloc_context3(mjpeg_dec);
    //         if (tmp_ctx) {
    //             avcodec_parameters_to_context(tmp_ctx, codec_params);
    //             if (avcodec_open2(tmp_ctx, mjpeg_dec, NULL) >= 0) {
    //                 AVPacket* pkt = av_packet_alloc();
    //                 AVFrame* frm = av_frame_alloc();
    //                 if (pkt && frm) {
    //                     av_new_packet(pkt, cam->image_data_size);
    //                     memcpy(pkt->data, cam->image_data, cam->image_data_size);
    //                     if (avcodec_send_packet(tmp_ctx, pkt) >= 0
    //                         && avcodec_receive_frame(tmp_ctx, frm) >= 0) {
    //                         cam->_width = frm->width;
    //                         cam->_height = frm->height;
    //                     }
    //                 }
    //                 av_frame_free(&frm);
    //                 av_packet_free(&pkt);
    //             }
    //             avcodec_free_context(&tmp_ctx);
    //         }
    //     }

    //     av_frame_free(&cam->_avu_input_frame);
    //     av_frame_free(&cam->_avu_output_frame);
    //     goto probe_cleanup;
    // }

    cam->_avc_decoder = avcodec_find_decoder(codec_params->codec_id);
    if (!cam->_avc_decoder) {
        result = WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER;
        goto probe_cleanup;
    }

    cam->_avc_decoder_ctx = avcodec_alloc_context3(cam->_avc_decoder);
    if (!cam->_avc_decoder_ctx) {
        result = WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER_CTX;
        goto probe_cleanup;
    }

    ret = avcodec_parameters_to_context(cam->_avc_decoder_ctx, codec_params);
    if (ret < 0) {
        result = _webcamize_result_from_avu_result(ret);
        goto probe_cleanup;
    }

    av_dict_set(&opts, "threads", "auto", 0);
    av_dict_set(&opts, "thread_type", "frame", 0);
    cam->_avc_decoder_ctx->thread_count = 0;
    cam->_avc_decoder_ctx->thread_type = FF_THREAD_FRAME;
    cam->_avc_decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    cam->_avc_decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    cam->_avc_decoder_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    cam->_avc_decoder_ctx->get_buffer2 = avcodec_default_get_buffer2;
    cam->_avc_decoder_ctx->lowres = 0;

    ret = avcodec_open2(cam->_avc_decoder_ctx, cam->_avc_decoder, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        result = _webcamize_result_from_avu_result(ret);
        goto probe_cleanup;
    }

    cam->_width = cam->_avc_decoder_ctx->width;
    cam->_height = cam->_avc_decoder_ctx->height;

probe_cleanup:
    if (format_ctx) avformat_close_input(&format_ctx);
    if (avio_ctx) {
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
    }
    if (result < WEBCAMIZE_RESULT_OK) goto error;
    if (cam->_no_convert) return WEBCAMIZE_RESULT_OK;

    // swscale setup
    result = _webcamize_setup_swscale(cam, cam->_avc_decoder_ctx->pix_fmt);
    if (result < WEBCAMIZE_RESULT_OK) goto error;
    return WEBCAMIZE_RESULT_OK;

error:
    if (cam->_sws_output_buffer) {
        av_free(cam->_sws_output_buffer);
        cam->_sws_output_buffer = NULL;
    }
    if (cam->_sws_ctx) {
        sws_freeContext(cam->_sws_ctx);
        cam->_sws_ctx = NULL;
    }
    if (cam->_avc_decoder_ctx) avcodec_free_context(&cam->_avc_decoder_ctx);
    if (cam->_avu_output_frame) av_frame_free(&cam->_avu_output_frame);
    if (cam->_avu_input_frame) av_frame_free(&cam->_avu_input_frame);
    return result;
}

static inline webcamize_result _webcamize_ffmpeg_convert(struct webcamize_camera* cam) {
    if (cam->_no_convert) {
        cam->output_data = (uint8_t*)cam->image_data;
        cam->output_data_size = cam->image_data_size;

        return WEBCAMIZE_RESULT_OK;
    };

    int ret;
    if (!cam->_avc_decoder_ctx) return WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_DECODER_CTX;

    if (!cam->_avc_packet) {
        cam->_avc_packet = av_packet_alloc();
        if (!cam->_avc_packet) return WEBCAMIZE_RESULT_CAM_NULL_AVCODEC_PACKET;
    }

    ret = av_new_packet(cam->_avc_packet, cam->image_data_size);
    if (ret < 0) return _webcamize_result_from_avu_result(ret);

    memcpy(cam->_avc_packet->data, cam->image_data, cam->image_data_size);

    ret = avcodec_send_packet(cam->_avc_decoder_ctx, cam->_avc_packet);
    if (ret < 0) {
        av_packet_unref(cam->_avc_packet);
        return _webcamize_result_from_avu_result(ret);
    }

    av_packet_unref(cam->_avc_packet);

    ret = avcodec_receive_frame(cam->_avc_decoder_ctx, cam->_avu_input_frame);
    if (ret < 0) return _webcamize_result_from_avu_result(ret);

    if (cam->_width != cam->_avu_input_frame->width
        || cam->_height != cam->_avu_input_frame->height) {
        cam->_width = cam->_avu_input_frame->width;
        cam->_height = cam->_avu_input_frame->height;
        webcamize_result sws_res
            = _webcamize_setup_swscale(cam, (enum AVPixelFormat)cam->_avu_input_frame->format);
        if (sws_res < WEBCAMIZE_RESULT_OK) return sws_res;
    }

    ret = sws_scale(cam->_sws_ctx, (const uint8_t* const*)cam->_avu_input_frame->data,
                    cam->_avu_input_frame->linesize, 0, cam->_height,
                    cam->_avu_output_frame->data, cam->_avu_output_frame->linesize);
    if (ret <= 0) return _webcamize_result_from_avu_result(ret);

    cam->output_data = cam->_sws_output_buffer;
    cam->output_data_size = cam->_sws_output_buffer_size;

    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result _webcamize_camera_gp2_feedframe(struct webcamize_camera* cam) {
    // gp_file_clean(cam->_gp2_file);

    int gp2_ret = gp_camera_capture_preview(cam->_gp2_camera, cam->_gp2_file, cam->_gp2_ctx);
    if (gp2_ret != GP_OK) return _webcamize_result_from_gp2_result(gp2_ret);

    gp2_ret
        = gp_file_get_data_and_size(cam->_gp2_file, &cam->image_data, &cam->image_data_size);
    if (gp2_ret != GP_OK) return _webcamize_result_from_gp2_result(gp2_ret);

    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result webcamize_camera_initialize(
    struct webcamize_context* ctx,
    struct webcamize_camera_initializer* cam_init,
    struct webcamize_camera** cam,
    int sink_fd,
    int target_fps,
    bool no_convert) {
    *cam = (struct webcamize_camera*)malloc(sizeof(struct webcamize_camera));
    if (!*cam) return _webcamize_result_from_errno(errno);

    memset(*cam, 0, sizeof(struct webcamize_camera));

    (*cam)->_sink_fd = sink_fd;
    (*cam)->_target_fps = target_fps;
    (*cam)->_no_convert = no_convert;
    (*cam)->_gp2_ctx = ctx->_gp2_context;
    atomic_store(&(*cam)->_stop, false);

    webcamize_result result = WEBCAMIZE_RESULT_OK;

    int gp2_ret = gp_camera_new(&(*cam)->_gp2_camera);
    if (gp2_ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(gp2_ret);
        goto error_free;
    }

    result = _webcamize_setup_gp2_camera(ctx, *cam, cam_init);
    if (result < WEBCAMIZE_RESULT_OK) goto error_camera;

    gp2_ret = gp_camera_init((*cam)->_gp2_camera, ctx->_gp2_context);
    if (gp2_ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(gp2_ret);
        goto error_camera;
    }

    gp2_ret = gp_file_new(&(*cam)->_gp2_file);
    if (gp2_ret < GP_OK) {
        result = _webcamize_result_from_gp2_result(gp2_ret);
        goto error_camera_exit;
    }

    result = _webcamize_camera_gp2_feedframe(*cam);
    if (result < WEBCAMIZE_RESULT_OK) goto error_file;

    result = _webcamize_camera_setup_ffmpeg(*cam);
    if (result < WEBCAMIZE_RESULT_OK) goto error_file;

    (*cam)->_initialized = true;
    return WEBCAMIZE_RESULT_OK;

error_file:
    gp_file_free((*cam)->_gp2_file);
error_camera_exit:
    gp_camera_exit((*cam)->_gp2_camera, ctx->_gp2_context);
error_camera:
    gp_camera_free((*cam)->_gp2_camera);
error_free:
    free(*cam);
    *cam = NULL;
    return result;
}

static inline int webcamize_camera_width(struct webcamize_camera* cam) { return cam->_width; }
static inline int webcamize_camera_height(struct webcamize_camera* cam) { return cam->_height; }

static inline webcamize_result webcamize_camera_feedframe(struct webcamize_camera* cam) {
    if (!cam->_initialized) return WEBCAMIZE_RESULT_CAMERA_NOT_INITIALIZED;

    int res = _webcamize_camera_gp2_feedframe(cam);
    if (res < WEBCAMIZE_RESULT_OK) return res;

    res = _webcamize_ffmpeg_convert(cam);
    if (res < WEBCAMIZE_RESULT_OK) return res;

    ssize_t n = write(cam->_sink_fd, cam->output_data, cam->output_data_size);
    if (n < 0) return _webcamize_result_from_errno(errno);

    return WEBCAMIZE_RESULT_OK;
}

static inline webcamize_result webcamize_camera_loop(struct webcamize_camera* cam) {
    webcamize_result res = WEBCAMIZE_RESULT_OK;
    struct timespec frame_start = {};
    struct timespec frame_end = {};
    struct timespec sleep_time = {};
    long frame_time = 0;
    long target_frame_time = 1000000000L / cam->_target_fps;

    while (!atomic_load(&cam->_stop)) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        res = webcamize_camera_feedframe(cam);
        if (res < WEBCAMIZE_RESULT_OK) return res;

        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        frame_time = (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L
                     + (frame_end.tv_nsec - frame_start.tv_nsec);
        if (frame_time < target_frame_time) {
            sleep_time.tv_sec = 0;
            sleep_time.tv_nsec = target_frame_time - frame_time;
            nanosleep(&sleep_time, NULL);
        }
    };

    return WEBCAMIZE_RESULT_OK;
}

static inline void webcamize_camera_stop(struct webcamize_camera* cam) {
    atomic_store(&cam->_stop, true);
}

static inline webcamize_result webcamize_camera_cleanup(struct webcamize_camera** cam) {
    if (!cam || !*cam) return WEBCAMIZE_RESULT_OK;

    if ((*cam)->_sws_output_buffer) av_free((*cam)->_sws_output_buffer);
    if ((*cam)->_sws_ctx) sws_freeContext((*cam)->_sws_ctx);
    if ((*cam)->_avu_output_frame) av_frame_free(&(*cam)->_avu_output_frame);
    if ((*cam)->_avu_input_frame) av_frame_free(&(*cam)->_avu_input_frame);
    if ((*cam)->_avc_decoder_ctx) avcodec_free_context(&(*cam)->_avc_decoder_ctx);
    if ((*cam)->_avc_packet) av_packet_free(&(*cam)->_avc_packet);

    if ((*cam)->_gp2_file) gp_file_free((*cam)->_gp2_file);
    if ((*cam)->_gp2_camera) {
        if ((*cam)->_gp2_ctx) gp_camera_exit((*cam)->_gp2_camera, (*cam)->_gp2_ctx);
        gp_camera_free((*cam)->_gp2_camera);
    }
    free(*cam);
    *cam = NULL;

    return WEBCAMIZE_RESULT_OK;
}
