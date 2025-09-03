#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/prctl.h>
#include <signal.h>

#define PWM_CHIP       "/sys/class/pwm/pwmchip1"
#define PWM_NUM        0  // ʹ�� pwm0
#define PWM_PERIOD_NS  20000000  // 20ms (50Hz)
#define SERVO_MIN_NS   1400000   // ����
#define SERVO_MAX_NS   2800000   // ����

// ���� PWM ͨ��
void enable_pwm_channel() {

    char channel_path[256];
    snprintf(channel_path, sizeof(channel_path), "%s/pwm%d", PWM_CHIP, PWM_NUM);

    // �ȼ��ͨ���Ƿ��Ѵ���
    if (access(channel_path, F_OK) == 0)
    {
        printf("PWM channel %d already exists\n", PWM_NUM);
        return; // ͨ���Ѵ��ڣ�ֱ�ӷ���
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

    // �ȴ��豸����
    usleep(100000);  // 100ms
}

// ���� PWM ����
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

// ����GStreamer����
void run_gstreamer()
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // ���ø������˳�ʱ�յ�SIGKILL
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1)
        {
            perror("prctl failed");
            _exit(1);
        }

        // �ٴμ�鸸�����Ƿ����˳��������Ա�̣�
        if (getppid() == 1)
        { // ��������̱��init����
            _exit(0);
        }
        // �ӽ���
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
    // �����̼���ִ��
}


int main() {
    // 1. ���� PWM ͨ��
    enable_pwm_channel();

    // 2. �������ں�ʹ��
    set_pwm_param("period", PWM_PERIOD_NS);
    set_pwm_param("enable", 1);

    run_gstreamer();

    // 3. ����ڶ�
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