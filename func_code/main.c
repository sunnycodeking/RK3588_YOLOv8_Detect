
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <turbojpeg.h>
#include <glib-unix.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>

#include "yolov8.h"
#include "image_utils.h"
#include "image_drawing.h"

#if defined(RV1106_1103)
#include "dma_alloc.hpp"
#endif

    /*-------------------------------------------
                      Globals
    -------------------------------------------*/
    static rknn_app_context_t rknn_app_ctx;
    static gboolean gst_initialized = FALSE;
    static GMainLoop *main_loop = NULL;

    // 显示管道全局变量
    static GstElement *display_pipeline = NULL;
    static GstElement *display_appsrc = NULL;
    static int display_width = 640;
    static int display_height = 480;

#define CAMERA_HEIGHT 640
#define CAMERA_WIDTH 480



    // 舵机控制参数
#define PWM_PATH0 "/sys/class/pwm/pwmchip0/pwm0/"
#define PWM_PATH1 "/sys/class/pwm/pwmchip1/pwm0/"
#define PWM_PERIOD_NS 20000000
#define MIN_DUTY_X_NS 1100000
#define MAX_DUTY_X_NS 3000000
#define MID_DUTY_X_NS 2100000
#define MIN_DUTY_Y_NS 1600000
#define MAX_DUTY_Y_NS 2400000
#define MID_DUTY_Y_NS 2000000
#define STEP_X_SIZE 50000
#define STEP_Y_SIZE 20000

    // 全局占空比变量
    static int current_duty_x = MID_DUTY_X_NS;
    static int current_duty_y = MID_DUTY_Y_NS;

    //超时时间
    static gint64 last_detection_time = 0;
#define TIMEOUT_NS (5 * G_TIME_SPAN_SECOND) // 5秒超时

    // 目标类别定义
#define PERSON_CLASS_ID 39
    static const char *PERSON_CLASS_NAME = "bottle";

    /*-------------------------------------------
                     偏移量结构体
    -------------------------------------------*/
    typedef struct
    {
        int x; // 水平偏移
        int y; // 垂直偏移
    } offset_t;

 
    /*-------------------------------------------
                   PWM控制函数
    -------------------------------------------*/
    static void set_pwm_duty(int duty_ns, const char *pwm_path)
    {
        static int initialized0 = 0;
        static int initialized1 = 0;
        char buffer[32];
        int fd;

        // 初始化PWM设备
        if (strcmp(pwm_path, PWM_PATH0) == 0 && !initialized0)
        {
            fd = open("/sys/class/pwm/pwmchip0/export", O_WRONLY);
            if (fd >= 0)
            {
                write(fd, "0", 1);
                close(fd);
            }

            fd = open(PWM_PATH0 "period", O_WRONLY);
            if (fd >= 0)
            {
                snprintf(buffer, sizeof(buffer), "%d", PWM_PERIOD_NS);
                write(fd, buffer, strlen(buffer));
                close(fd);
            }

            fd = open(PWM_PATH0 "enable", O_WRONLY);
            if (fd >= 0)
            {
                write(fd, "1", 1);
                close(fd);
            }
            initialized0 = 1;
        }
        else if (strcmp(pwm_path, PWM_PATH1) == 0 && !initialized1)
        {
            fd = open("/sys/class/pwm/pwmchip1/export", O_WRONLY);
            if (fd >= 0)
            {
                write(fd, "0", 1);
                close(fd);
            }

            fd = open(PWM_PATH1 "period", O_WRONLY);
            if (fd >= 0)
            {
                snprintf(buffer, sizeof(buffer), "%d", PWM_PERIOD_NS);
                write(fd, buffer, strlen(buffer));
                close(fd);
            }

            fd = open(PWM_PATH1 "enable", O_WRONLY);
            if (fd >= 0)
            {
                write(fd, "1", 1);
                close(fd);
            }
            initialized1 = 1;
        }

        // 限制占空比范围
        duty_ns = duty_ns < 0 ? 0 : duty_ns;
        duty_ns = duty_ns > PWM_PERIOD_NS ? PWM_PERIOD_NS : duty_ns;

        // 设置占空比
        char duty_path[256];
        snprintf(duty_path, sizeof(duty_path), "%sduty_cycle", pwm_path);
        fd = open(duty_path, O_WRONLY);
        if (fd >= 0)
        {
            snprintf(buffer, sizeof(buffer), "%d", duty_ns);
            write(fd, buffer, strlen(buffer));
            close(fd);
            //printf("[PWM] Set %s duty: %d ns\n", pwm_path, duty_ns);
        }
        else
        {
            perror("Failed to set PWM duty");
        }
    }
    /*-------------------------------------------
                  禁用PWM输出函数
    -------------------------------------------*/
    static void disable_pwm()
    {
        int fd;
        const char *disable_value = "0";

        // 禁用PWM0
        fd = open("/sys/class/pwm/pwmchip0/pwm0/enable", O_WRONLY);
        if (fd >= 0)
        {
            write(fd, disable_value, 1);
            close(fd);
            printf("[PWM] Disabled pwmchip0/pwm0\n");
        }
        else
        {
            perror("Failed to disable pwmchip0/pwm0");
        }

        // 禁用PWM1
        fd = open("/sys/class/pwm/pwmchip1/pwm0/enable", O_WRONLY);
        if (fd >= 0)
        {
            write(fd, disable_value, 1);
            close(fd);
            //printf("[PWM] Disabled pwmchip1/pwm0\n");
        }
        else
        {
            perror("Failed to disable pwmchip1/pwm0");
        }
    }
/*-------------------------------------------
                  信号处理函数
-------------------------------------------*/
    static gboolean handle_signal(gpointer user_data)
    {
        GMainLoop *loop = (GMainLoop *)user_data;
        g_print("\nStopping gracefully...\n");

        // 设置舵机到中间位置
        set_pwm_duty(MID_DUTY_Y_NS, PWM_PATH0);
        set_pwm_duty(MID_DUTY_X_NS, PWM_PATH1);

        // 禁用PWM输出
        disable_pwm();

        g_main_loop_quit(loop);
        return TRUE;
    }

    /*-------------------------------------------
              舵机控制函数
-------------------------------------------*/
    /*static void control_servos(offset_t offset)
    {
         int current_duty_x = MID_DUTY_X_NS;
         int current_duty_y = MID_DUTY_Y_NS;

        // 水平方向控制
        if (abs(offset.x) >= 100)
        {
            current_duty_x += ((offset.x > 0) ? 1 : -1) * STEP_X_SIZE;

            if (current_duty_x >= MAX_DUTY_X_NS)
            {
                current_duty_x = MAX_DUTY_X_NS;
            }
            else if (current_duty_x <= MIN_DUTY_X_NS)
            {
                current_duty_x = MIN_DUTY_X_NS;
            }

            set_pwm_duty(current_duty_x, PWM_PATH1);
        }

        // 垂直方向控制
        if (abs(offset.y) >= 80)
        {
            current_duty_y += ((offset.y > 0) ? -1 : 1) * STEP_Y_SIZE;

            if (current_duty_y >= MAX_DUTY_Y_NS)
            {
                current_duty_y = MAX_DUTY_Y_NS;
            }
            else if (current_duty_y <= MIN_DUTY_Y_NS)
            {
                current_duty_y = MIN_DUTY_Y_NS;
            }

            set_pwm_duty(current_duty_y, PWM_PATH0);
        }
    }*/

/*-------------------------------------------
              GStreamer Callbacks
-------------------------------------------*/
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug_info = NULL;
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("Error: %s\nDebug info: %s\n", err->message, debug_info);
            g_clear_error(&err);
            g_free(debug_info);
            g_main_loop_quit(main_loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(main_loop);
            break;
        default: break;
    }
    return TRUE;
}

/*-------------------------------------------
              JPEG 内存解码函数
-------------------------------------------*/
static int decode_jpeg_memory(const unsigned char *jpeg_data, unsigned long jpeg_size, image_buffer_t *image) {
    int width, height;
    int origin_width, origin_height;
    int subsample, colorspace;
    unsigned char *rgb_buf = NULL;
    tjhandle handle = NULL;
    int flags = 0;
    int ret = 0;

    // 初始化 TurboJPEG 解码器
    handle = tjInitDecompress();
    if (!handle) {
        g_printerr("tjInitDecompress failed: %s\n", tjGetErrorStr());
        return -1;
    }

    // 获取 JPEG 头部信息
    ret = tjDecompressHeader3(handle, jpeg_data, jpeg_size, &origin_width, &origin_height, &subsample, &colorspace);
    if (ret < 0) {
        g_printerr("JPEG header error: %s\n", tjGetErrorStr());
        tjDestroy(handle);
        return -1;
    }

    // 创建输出缓冲区
    width = origin_width;
    height = origin_height;
    unsigned long rgb_size = width * height * 3; // RGB888 格式
    rgb_buf = (unsigned char *)malloc(rgb_size);
    if (!rgb_buf) {
        g_printerr("Memory allocation failed for RGB buffer\n");
        tjDestroy(handle);
        return -1;
    }

    // 解码 JPEG 到 RGB888
    ret = tjDecompress2(handle, jpeg_data, jpeg_size, rgb_buf, 
                        width, 0, height, TJPF_RGB, flags);
    if (ret < 0) {
        g_printerr("JPEG decompress failed: %s\n", tjGetErrorStr());
        free(rgb_buf);
        tjDestroy(handle);
        return -1;
    }

    // 设置图像参数
    image->width = width;
    image->height = height;
    image->format = IMAGE_FORMAT_RGB888;
    image->virt_addr = rgb_buf;
    image->size = rgb_size;
    
    tjDestroy(handle);
    return 0;
}

/*-------------------------------------------
              创建显示管道
-------------------------------------------*/
int create_display_pipeline(int width, int height, const char *host, int port)
{
    display_width = width;
    display_height = height;

    display_pipeline = gst_pipeline_new("rtp-stream-pipeline");
    display_appsrc = gst_element_factory_make("appsrc", "display-source");
    GstElement *videoconvert = gst_element_factory_make("videoconvert", "convert");
    // 在 videoconvert 和 jpegenc 之间插入 capsfilter
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "jpeg-caps");
    GstCaps *jpeg_caps = gst_caps_new_simple("video/x-raw",
                                             "format", G_TYPE_STRING, "I420", // 标准 YUV 格式，兼容 JPEG 编码
                                             NULL);

    GstElement *jpegenc = gst_element_factory_make("jpegenc", "jpeg-encoder");
    GstElement *rtpjpegpay = gst_element_factory_make("rtpjpegpay", "rtp-payload");
    GstElement *udpsink = gst_element_factory_make("udpsink", "udp-sink");
    

    if (!display_pipeline || !display_appsrc || !videoconvert || !jpegenc || !rtpjpegpay || !udpsink)
    {
        g_printerr("Failed to create RTP pipeline elements\n");
        return -1;
    }

    // 配置 appsrc
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "format", G_TYPE_STRING, "RGB",
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height,
                                        "framerate", GST_TYPE_FRACTION, 15, 1,
                                        NULL);
    g_object_set(display_appsrc,
                 "caps", caps,
                 "format", GST_FORMAT_TIME,
                 "block", TRUE,
                 "stream-type", 0, // GST_APP_STREAM_TYPE_STREAM
                 "is-live", TRUE,
                 NULL);
    gst_caps_unref(caps);

    // 配置 JPEG 编码器
    g_object_set(jpegenc,
                 "quality", 85,    // 质量 85%
                 "idct-method", 0, // 使用默认 DCT 算法
                 NULL);

    g_object_set(capsfilter, "caps", jpeg_caps, NULL);
    gst_caps_unref(jpeg_caps);

    // 配置 RTP 负载
    g_object_set(rtpjpegpay, "pt", 26, NULL);

    

    // 配置 UDP 发送
    g_object_set(udpsink,
                 "host", "127.0.0.1",
                 "port", 5000,
                 "sync", FALSE,  // 禁用同步
                 "async", FALSE, // 禁用异步
                 NULL);

    // 更新管道链接顺序
    gst_bin_add_many(GST_BIN(display_pipeline),
                     display_appsrc, videoconvert, capsfilter, jpegenc, rtpjpegpay, udpsink, NULL);
    if (!gst_element_link_many(display_appsrc, videoconvert, capsfilter, jpegenc, rtpjpegpay, udpsink, NULL))
    {
        g_printerr("Failed to link elements!\n");
        gst_object_unref(display_pipeline);
        return -1;
    }

    // 启动管道
    GstStateChangeReturn ret = gst_element_set_state(display_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Failed to start RTP pipeline: %s\n",
                   gst_element_state_change_return_get_name(ret));
        gst_object_unref(display_pipeline);
        return -1;
    }

    return 0;
}

/*-------------------------------------------
              推送图像到显示管道
-------------------------------------------*/
static void push_frame_to_display(const image_buffer_t *image)
{
    if (!display_appsrc)
    {
        g_printerr("Error: display_appsrc is NULL!\n");
        return;
    }

    // 打印图像信息
    /* g_print("[DEBUG] Pushing frame: size=%d, width=%d, height=%d, format=%s\n",
             image->size, image->width, image->height,
             (image->format == IMAGE_FORMAT_RGB888) ? "RGB888" : "Unknown");*/

    // 检查 appsrc 状态
    GstState state;
    GstState pending;
    GstStateChangeReturn ret = gst_element_get_state(
        display_appsrc, &state, &pending, GST_CLOCK_TIME_NONE);

    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Failed to get appsrc state!\n");
    }
    /*else
    {
        g_print("[DEBUG] Appsrc state: %s (pending: %s)\n",
                gst_element_state_get_name(state),
                gst_element_state_get_name(pending));
    }
    */

        // 创建 GStreamer 缓冲区
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, image->size, NULL);
    if (!buffer)
    {
        g_printerr("Failed to allocate buffer!\n");
        return;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE))
    {
        // 拷贝图像数据到缓冲区
        memcpy(map.data, image->virt_addr, image->size);
        gst_buffer_unmap(buffer, &map);

        // 设置时间戳
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(
            g_get_monotonic_time(), GST_SECOND, G_USEC_PER_SEC);

        // 推送缓冲区
        GstFlowReturn flow_ret;
        g_signal_emit_by_name(display_appsrc, "push-buffer", buffer, &flow_ret);

        if (flow_ret != GST_FLOW_OK)
        {
            g_printerr("Failed to push buffer: %s\n",
                       gst_flow_get_name(flow_ret));
        }
        /*else
        {
            g_print("[DEBUG] Buffer pushed successfully!\n");
        }*/

        gst_buffer_unref(buffer);
    }
    else
    {
        g_printerr("Failed to map buffer for writing!\n");
        gst_buffer_unref(buffer);
    }
    FILE *fp = fopen("debug.jpg", "wb");
    if (fp)
    {
        fwrite(image->virt_addr, 1, image->size, fp);
        fclose(fp);
        g_print("Saved JPEG frame to debug.jpg\n");
    }
}

/*-------------------------------------------
              查找最高概率的瓶子目标
-------------------------------------------*/
static object_detect_result* find_best_person(object_detect_result_list *results) {
    object_detect_result* best_person = NULL;
    float max_prob = 0.0;
    
    for (int i = 0; i < results->count; i++) {
        object_detect_result *det_result = &(results->results[i]);
        
        // 检查是否为person目标
        if (det_result->cls_id == PERSON_CLASS_ID || 
            strcmp(coco_cls_to_name(det_result->cls_id), PERSON_CLASS_NAME) == 0) {
            
            // 检查是否是最高概率的person
            if (det_result->prop > max_prob) {
                max_prob = det_result->prop;
                best_person = det_result;
            }
        }
    }
    
    return best_person;
}

/*-------------------------------------------
        计算检测框中心到屏幕中心的偏移量
-------------------------------------------*/
static offset_t calculate_offset(const object_detect_result *detection, int screen_width, int screen_height) {
    offset_t offset = {0, 0};
    
    if (!detection) return offset;
    
    // 计算检测框中心坐标
    int box_center_x = (detection->box.left + detection->box.right) / 2;
    int box_center_y = (detection->box.top + detection->box.bottom) / 2;
    
    // 计算屏幕中心坐标
    int screen_center_x = screen_width / 2;
    int screen_center_y = screen_height / 2;
    
    // 计算偏移量
    offset.x = box_center_x - screen_center_x;
    offset.y = box_center_y - screen_center_y;
    
    return offset;
}
/*-------------------------------------------
        计算舵机输出占空比
---------------------------------------------*/
static void calculate_dutyset(offset_t offset)
{
    static int current_duty_x = MID_DUTY_X_NS;
    static int current_duty_y = MID_DUTY_Y_NS;

    // 检查偏移量是否移出了中心区域（即无偏移）
    if (abs(offset.x) >= 100)
    {
        current_duty_x = current_duty_x + ((offset.x > 0) ? 1 : ((offset.x < 0) ? -1 : 0)) * STEP_X_SIZE;

        if (current_duty_x >= MAX_DUTY_X_NS)
        {
            current_duty_x = MAX_DUTY_X_NS;
        }
        else if (current_duty_x <= MIN_DUTY_X_NS)
        {
            current_duty_x = MIN_DUTY_X_NS;
        }

        // 直接设置X轴舵机占空比
        set_pwm_duty(current_duty_x, PWM_PATH1);
    }

    if (abs(offset.y) >= 80)
    {
        current_duty_y = current_duty_y + (-1) * ((offset.y > 0) ? 1 : ((offset.y < 0) ? -1 : 0)) * STEP_Y_SIZE;

        if (current_duty_y >= MAX_DUTY_Y_NS)
        {
            current_duty_y = MAX_DUTY_Y_NS;
        }
        else if (current_duty_y <= MIN_DUTY_Y_NS)
        {
            current_duty_y = MIN_DUTY_Y_NS;
        }

        // 直接设置Y轴舵机占空比
        set_pwm_duty(current_duty_y, PWM_PATH0);
    }

    //printf("[PWM] Set X duty cycle: %d ns, Y duty cycle: %d ns\n", current_duty_x, current_duty_y);
}

/*-------------------------------------------
        绘制屏幕中心十字线和偏移信息
-------------------------------------------*/
static void draw_center_and_offset(image_buffer_t *image, const offset_t *offset)
{
    // 获取屏幕中心
    int center_x = image->width / 2;
    int center_y = image->height / 2;

    // 绘制中心十字线
    int cross_size = 20;
    draw_line(image, center_x - cross_size, center_y, center_x + cross_size, center_y, COLOR_GREEN, 2);
    draw_line(image, center_x, center_y - cross_size, center_x, center_y + cross_size, COLOR_GREEN, 2);

    // 绘制偏移信息
    char offset_text[128];
    sprintf(offset_text, "Offset: X=%d, Y=%d", offset->x, offset->y);
    draw_text(image, offset_text, 10, 30, COLOR_GREEN, 12);

    // 绘制舵机占空比信息（需要从静态变量获取）
    static int last_duty_x = MID_DUTY_X_NS;
    static int last_duty_y = MID_DUTY_Y_NS;
    char dutyset_text[128];
    sprintf(dutyset_text, "Dutyset: X=%d, Y=%d", last_duty_x, last_duty_y);
    draw_text(image, dutyset_text, 10, 50, COLOR_GREEN, 12);
}

static GstFlowReturn new_sample_callback(GstAppSink *appsink, gpointer user_data) {
    static guint64 frame_count = 0;
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) {
        g_printerr("Failed to pull sample from appsink\n");
        return GST_FLOW_ERROR;
    }

    // 获取当前时间
    gint64 current_time = g_get_monotonic_time();

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        g_printerr("Failed to get buffer from sample\n");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        g_printerr("Failed to map buffer\n");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    // 创建图像缓冲区
    image_buffer_t src_image;
    memset(&src_image, 0, sizeof(image_buffer_t));
    
    // 解码 JPEG 数据
    int ret = decode_jpeg_memory(map.data, map.size, &src_image);
    if (ret != 0) {
        g_printerr("JPEG decode failed: %d\n", ret);
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    
    // 执行目标检测
    object_detect_result_list od_results;
    memset(&od_results, 0, sizeof(object_detect_result_list));
    
    ret = inference_yolov8_model(&rknn_app_ctx, &src_image, &od_results);
    if (ret != 0) {
        g_printerr("Inference failed: %d\n", ret);
    } else {   
        // 查找最高概率的person目标
        object_detect_result* best_person = find_best_person(&od_results);
        
        if (best_person) {
            // 更新最后检测时间
            last_detection_time = current_time;
            // 计算偏移量
            offset_t offset = calculate_offset(best_person, src_image.width, src_image.height);
            calculate_dutyset( offset );

            // 打印检测信息和偏移量
            printf("Best person: %.1f%% @ (%d %d %d %d) | Offset: X=%d, Y=%d  |  Dutyset:  X=%d, Y=%d\n",
                   best_person->prop * 100,
                   best_person->box.left, best_person->box.top,
                   best_person->box.right, best_person->box.bottom,
                   offset.x, offset.y,
                   current_duty_x, current_duty_y);

            // 绘制检测结果
            char text[256];
            int x1 = best_person->box.left;
            int y1 = best_person->box.top;
            int x2 = best_person->box.right;
            int y2 = best_person->box.bottom;

            draw_rectangle(&src_image, x1, y1, x2 - x1, y2 - y1, COLOR_BLUE, 3);

            sprintf(text, "Bottle %.1f%%", best_person->prop * 100);
            draw_text(&src_image, text, x1, y1 - 20, COLOR_RED, 10);
            
            // 绘制中心十字线和偏移信息
            draw_center_and_offset(&src_image, &offset);
        } else {
            g_print("No Bottle detected in this frame\n");

            // 检查是否超时
            if (last_detection_time > 0 &&
                (current_time - last_detection_time) > TIMEOUT_NS)
            {
                g_print("Timeout reached, resetting servos to middle position\n");

                // 重置舵机位置
                current_duty_x = MID_DUTY_X_NS;
                current_duty_y = MID_DUTY_Y_NS;
                set_pwm_duty(current_duty_x, PWM_PATH1);
                set_pwm_duty(current_duty_y, PWM_PATH0);

                // 重置时间戳避免重复设置
                last_detection_time = 0;
            }
        }
        
        // 推送处理后的图像到显示管道
        push_frame_to_display(&src_image);
    }

    // 释放资源
    free(src_image.virt_addr); // 释放解码后的图像内存
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    
    // 限制帧率（可选）
    frame_count++;
    if (frame_count % 10 == 0) {
        g_print("Processed %llu frames\n", frame_count);
    }
    
    return GST_FLOW_OK;
}

/*-------------------------------------------
              Model Initialization
-------------------------------------------*/
int init_detection_model(const char *model_path) {
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));
    
    init_post_process();
    
    int ret = init_yolov8_model(model_path, &rknn_app_ctx);
    if (ret != 0) {
        g_printerr("Model initialization failed: %d\n", ret);
        return -1;
    }
    
    return 0;
}

/*-------------------------------------------
            Pipeline Initialization
-------------------------------------------*/
GstElement* create_capture_pipeline(const char* device, int width, int height, int fps) {
    GstElement *pipeline = gst_pipeline_new("capture-pipeline");
    GstElement *src = gst_element_factory_make("v4l2src", "source");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "caps");
    GstElement *jpegenc = gst_element_factory_make("jpegenc", "encoder");
    GstElement *jpegparse = gst_element_factory_make("jpegparse", "parser");
    GstElement *appsink = gst_element_factory_make("appsink", "sink");
    
    if (!pipeline || !src || !capsfilter || !jpegenc || !jpegparse || !appsink) {
        g_printerr("Failed to create capture pipeline elements\n");
        if (pipeline) gst_object_unref(pipeline);
        return NULL;
    }

    // 配置摄像头参数
    g_object_set(src,
        "device", device,
        "io-mode", 2,  // 内存映射模式
        NULL);

    // 配置格式过滤器
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, width,
        "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, fps, 1,
        NULL);
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    // 配置JPEG编码器
    g_object_set(jpegenc, "quality", 85, NULL);  // 设置JPEG质量
    
    // 配置appsink
    g_object_set(appsink, 
        "emit-signals", TRUE, 
        "sync", FALSE,
        "max-buffers", 1,  // 减少缓冲
        "drop", TRUE,      // 当处理不及时时丢弃帧
        NULL);
    
    g_signal_connect(appsink, "new-sample", G_CALLBACK(new_sample_callback), NULL);
    
    // 构建管道
    gst_bin_add_many(GST_BIN(pipeline), 
        src, capsfilter, jpegenc, jpegparse, appsink, NULL);
    
    // 链接元件
    if (!gst_element_link_many(src, capsfilter, jpegenc, jpegparse, appsink, NULL)) {
        g_printerr("Failed to link capture pipeline elements\n");
        gst_object_unref(pipeline);
        return NULL;
    }

    return pipeline;
}

/*-------------------------------------------
                  清理函数
-------------------------------------------*/
void cleanup_display_pipeline()
{
    // 重置舵机位置
    set_pwm_duty(MID_DUTY_X_NS, PWM_PATH1);
    set_pwm_duty(MID_DUTY_Y_NS, PWM_PATH0);


    // 禁用PWM输出
    disable_pwm();

    if (display_pipeline)
    {
        gst_element_set_state(display_pipeline, GST_STATE_NULL);
        gst_object_unref(display_pipeline);
        display_pipeline = NULL;
        display_appsrc = NULL;
    }
}

/*-------------------------------------------
                  Main Function
-------------------------------------------*/
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <model_path>\n", argv[0]);
        return -1;
    }

    // 初始化GStreamer
    gst_init(&argc, &argv);
    gst_initialized = TRUE;

    // 初始化目标检测模型
    if (init_detection_model(argv[1]) != 0)
    {
        return -1;
    }

    // 初始化最后检测时间
    last_detection_time = g_get_monotonic_time();


    // 创建显示管道
    int display_width = 640;
    int display_height = 480;
    const char *client_ip = "127.0.0.1"; // 回环测试版本
    int client_port = 5000;                  // 客户端端口
    if (create_display_pipeline(display_width, display_height, client_ip, client_port) != 0)
    {
        g_printerr("Failed to create RTP pipeline\n");
        return -1;
    }

    // 创建主循环
    main_loop = g_main_loop_new(NULL, FALSE);
    // 将 SIGINT 信号绑定到 handle_signal 函数，并在信号触发时传入 main_loop 对象，实现程序的优雅退出​​。
    // SIGINT：终端中断信号（通常是 Ctrl+C 触发）。SIGTERM：终止信号（如 kill 命令默认发送的信号）。
    g_unix_signal_add(SIGINT, handle_signal, main_loop);
    g_unix_signal_add(SIGTERM, handle_signal, main_loop);
    g_unix_signal_add(SIGQUIT, handle_signal, main_loop);

    // 创建捕获管道
    const char *camera_device = "/dev/video11";
    int width = CAMERA_HEIGHT;
    int height = CAMERA_WIDTH;
    int fps = 30;

    GstElement *capture_pipeline = create_capture_pipeline(camera_device, width, height, fps);
    if (!capture_pipeline)
    {
        g_printerr("Failed to create capture pipeline\n");
        cleanup_display_pipeline();
        return -1;
    }

    // 设置总线监控
    GstBus *bus = gst_element_get_bus(capture_pipeline);
    gst_bus_add_watch(bus, bus_callback, main_loop);
    gst_object_unref(bus);

    // 启动管道
    GstStateChangeReturn ret = gst_element_set_state(capture_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Failed to start capture pipeline\n");
        gst_object_unref(capture_pipeline);
        cleanup_display_pipeline();
        return -1;
    }

    g_print("Starting object detection and tracking system...\n");
    g_print("Press Ctrl+C to exit\n");

    g_main_loop_run(main_loop);

    // 清理资源
    g_print("Stopping pipelines...\n");
    gst_element_set_state(capture_pipeline, GST_STATE_NULL);
    gst_object_unref(capture_pipeline);

    cleanup_display_pipeline();
    release_yolov8_model(&rknn_app_ctx);
    deinit_post_process();

    g_main_loop_unref(main_loop);
    g_print("Application exited cleanly\n");
    return 0;
}
