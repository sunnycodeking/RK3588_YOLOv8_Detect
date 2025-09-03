#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/prctl.h>
#include <signal.h>

#define PWM_CHIP       "/sys/class/pwm/pwmchip1"
#define PWM_NUM        0  // 使用 pwm0
#define PWM_PERIOD_NS  20000000  // 20ms (50Hz)
#define SERVO_MIN_NS   1400000   // 最左
#define SERVO_MAX_NS   2800000   // 最右

// 启用 PWM 通道
void enable_pwm_channel() {

    char channel_path[256];
    snprintf(channel_path, sizeof(channel_path), "%s/pwm%d", PWM_CHIP, PWM_NUM);

    // 先检查通道是否已存在
    if (access(channel_path, F_OK) == 0)
    {
        printf("PWM channel %d already exists\n", PWM_NUM);
        return; // 通道已存在，直接返回
    }



    char export_path[256];
    snprintf(export_path, sizeof(export_path), "%s/export", PWM_CHIP);

    
    FILE* fp = fopen(export_path, "w");
    
     
    if (fp == NULL) {
        perror("Failed to export PWM");
        exit(1);
    }
    fprintf(fp, "%d", PWM_NUM);
    fclose(fp);

    // 等待设备生成
    usleep(100000);  // 100ms
}

// 设置 PWM 参数
void set_pwm_param(const char* param, int value) {
    char path[256];
    snprintf(path, sizeof(path), "%s/pwm%d/%s", PWM_CHIP, PWM_NUM, param);

    FILE* fp = fopen(path, "w");
    if (fp == NULL) {
        perror("Failed to set PWM parameter");
        exit(1);
    }
    fprintf(fp, "%d", value);
    fclose(fp);
}

// 运行GStreamer命令
void run_gstreamer()
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // 设置父进程退出时收到SIGKILL
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1)
        {
            perror("prctl failed");
            _exit(1);
        }

        // 再次检查父进程是否已退出（防御性编程）
        if (getppid() == 1)
        { // 如果父进程变成init进程
            _exit(0);
        }
        // 子进程
        char *args[] = {
            "gst-launch-1.0",
            "v4l2src", "device=/dev/video11",
            "!", "video/x-raw,format=NV12,width=320,height=240,framerate=15/1",
            "!", "videoconvert",
            "!", "jpegenc", "quality=50",
            "!", "rtpjpegpay", "pt=26",
            "!", "udpsink", "host=192.168.137.254", "port=5000",
            NULL};

        execvp("gst-launch-1.0", args);
        perror("Failed to execute gst-launch-1.0");
        exit(1);
    }
    else if (pid < 0)
    {
        perror("Failed to fork");
        exit(1);
    }
    // 父进程继续执行
}


int main() {
    // 1. 启用 PWM 通道
    enable_pwm_channel();

    // 2. 配置周期和使能
    set_pwm_param("period", PWM_PERIOD_NS);
    set_pwm_param("enable", 1);

    run_gstreamer();

    // 3. 舵机摆动
    while (1) {
        for (int duty = SERVO_MIN_NS; duty <= SERVO_MAX_NS; duty += 50000) {
            set_pwm_param("duty_cycle", duty);
            usleep(100000);
        }
        for (int duty = SERVO_MAX_NS; duty >= SERVO_MIN_NS; duty -= 50000) {
            set_pwm_param("duty_cycle", duty);
            usleep(100000);
        }
    }

    return 0;
}