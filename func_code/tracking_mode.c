#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#define SHM_KEY 0x1234
#define SHM_SIZE sizeof(shm_data_t)
#define SAFE_DUTY_X_NS 2100000
#define SAFE_DUTY_Y_NS 2000000
#define TIMEOUT_SEC 5
#define PWM_PATH0 "/sys/class/pwm/pwmchip0/pwm0/"
#define PWM_PATH1 "/sys/class/pwm/pwmchip1/pwm0/"
#define PWM_PERIOD_NS 20000000

typedef struct
{
    int duty_x; 
    int duty_y; 
    time_t last_update;
} shm_data_t;

volatile int keep_running = 1;
shm_data_t *shared_data = NULL;
int shmid = -1;

void handle_signal(int sig)
{
    printf("\n[Receiver] Cleaning up before exit...\n");
    set_pwm_duty(0, PWM_PATH0); // 舵机0归零
    set_pwm_duty(0, PWM_PATH1); // 舵机1归零
    keep_running = 0;
}

int is_sender_alive()
{
    if (!shared_data)
    {
        printf("No data received in %d seconds\n", TIMEOUT_SEC);
        return 0;
    }
    return (time(NULL) - shared_data->last_update) < TIMEOUT_SEC;
}

void set_pwm_duty(int duty_ns, const char *pwm_path)
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
        printf("[PWM] Set %s duty: %d ns\n", pwm_path, duty_ns);
    }
    else
    {
        perror("Failed to set PWM duty");
    }
}

void init_shared_memory()
{
    if ((shmid = shmget(SHM_KEY, SHM_SIZE, 0666)) == -1)
    {
        perror("shmget failed");
        exit(EXIT_FAILURE);
    }

    if ((shared_data = (shm_data_t *)shmat(shmid, NULL, 0)) == (void *)-1)
    {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }
}

void cleanup()
{
    if (shared_data)
        shmdt(shared_data);
    printf("[Receiver] Resources released\n");
}

int main()
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    init_shared_memory();
    printf("[Receiver] Started. Waiting for dual servo data...\n");

    while (keep_running)
    {
        if (is_sender_alive())
        {
            set_pwm_duty(shared_data->duty_x, PWM_PATH0);
            set_pwm_duty(shared_data->duty_y, PWM_PATH1);
        }
        else
        {
            printf("[Warning] Sender timeout! Using safe duty\n");
            set_pwm_duty(SAFE_DUTY_X_NS, PWM_PATH0);
            set_pwm_duty(SAFE_DUTY_Y_NS, PWM_PATH1);
        }
        usleep(100000); // 0.1s控制周期
    }

    cleanup();
    return 0;
}