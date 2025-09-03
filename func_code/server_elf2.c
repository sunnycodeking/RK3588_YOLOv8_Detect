#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define SERVER_PORT 8888
#define BACKLOG 10
#define PWM_CHIP "/sys/class/pwm/pwmchip1/"
#define PWM_NUM 0              // using pwm0
#define PWM_PERIOD_NS 20000000 // 20ms (50Hz)
#define MAX_CHILD_PROCESSES 10
pid_t g_servo_pid = 0;

typedef struct
{
    pid_t pid;
    int client_fd;
} ChildProcess;


ChildProcess g_child_processes[MAX_CHILD_PROCESSES]; 
int g_child_count = 0;//计数器，记录子进程数量

void add_child_process(pid_t pid, int client_fd)
{
    if (g_child_count < MAX_CHILD_PROCESSES)
    {
        g_child_processes[g_child_count].pid = pid;
        g_child_processes[g_child_count].client_fd = client_fd;
        g_child_count++;
    }
}

void remove_child_process(pid_t pid)
{
    for (int i = 0; i < g_child_count; i++)
    {
        if (g_child_processes[i].pid == pid)
        {
            // Shift elements to fill the gap
            for (int j = i; j < g_child_count - 1; j++)
            {
                g_child_processes[j] = g_child_processes[j + 1];
            }
            g_child_count--;
            break;
        }
    }
}

void stop_child_processes(int client_fd)
{
    printf("Stopping child processes for client fd %d...\n", client_fd);
    for (int i = 0; i < g_child_count; i++)
    {
        if (g_child_processes[i].client_fd == client_fd)
        {
            printf("Stopping PID %d\n", g_child_processes[i].pid);
            kill(g_child_processes[i].pid, SIGTERM);
            waitpid(g_child_processes[i].pid, NULL, 0);
            remove_child_process(g_child_processes[i].pid);
            i--; // Adjust index after removal
        }
    }
}

void stop_specific_process(int client_fd, const char *process_name)
{
    printf("Stopping %s process for client fd %d...\n", process_name, client_fd);
    for (int i = 0; i < g_child_count; i++)
    {
        if (g_child_processes[i].client_fd == client_fd)
        {
            char proc_path[256];
            snprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline", g_child_processes[i].pid);

            FILE *fp = fopen(proc_path, "r");
            if (fp)
            {
                char cmdline[256];
                if (fgets(cmdline, sizeof(cmdline), fp))
                {
                    // 修改为部分匹配
                    if (strstr(cmdline, process_name) != NULL)
                    {
                        printf("Stopping PID %d (%s)\n", g_child_processes[i].pid, cmdline);
                        kill(g_child_processes[i].pid, SIGTERM);
                        waitpid(g_child_processes[i].pid, NULL, 0);
                        remove_child_process(g_child_processes[i].pid);
                        i--; // 调整索引
                    }
                }
                fclose(fp);
            }
        }
    }
}
void sigchld_handler(int sig)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        remove_child_process(pid);
        if (pid == g_servo_pid)
        {
            g_servo_pid = 0;
        }
    }
}

void stop_servo_process()
{
    if (g_servo_pid > 0)
    {
        kill(g_servo_pid, SIGTERM);
        waitpid(g_servo_pid, NULL, 0);
        g_servo_pid = 0;
    }
}

void servo90()
{
    stop_servo_process();
    char export_path[256];
    snprintf(export_path, sizeof(export_path), "%s/export", PWM_CHIP);

    FILE *fp = fopen(export_path, "w");
    if (fp == NULL)
    {
        perror("Failed to export PWM");
        exit(1);
    }
    fprintf(fp, "%d", PWM_NUM);
    fclose(fp);

    usleep(100000);

    char path_period[256];
    snprintf(path_period, sizeof(path_period), "%s/pwm0/period", PWM_CHIP);

    FILE *fp1 = fopen(path_period, "w");
    if (fp1 == NULL)
    {
        perror("Failed to set period");
        exit(1);
    }
    fprintf(fp1, "%d", 20000000);
    fclose(fp1);
    usleep(10000);

    char path_duty_cycle[256];
    snprintf(path_duty_cycle, sizeof(path_duty_cycle), "%s/pwm0/duty_cycle", PWM_CHIP);

    FILE *fp2 = fopen(path_duty_cycle, "w");
    if (fp2 == NULL)
    {
        perror("Failed to set duty cycle");
        exit(1);
    }
    fprintf(fp2, "%d", 1800000);
    fclose(fp2);
    usleep(10000);

    char path_enable[256];
    snprintf(path_enable, sizeof(path_enable), "%s/pwm0/enable", PWM_CHIP);

    FILE *fp3 = fopen(path_enable, "w");
    if (fp3 == NULL)
    {
        perror("Failed to set enable");
        exit(1);
    }
    fprintf(fp3, "%d", 1);
    fclose(fp3);
    usleep(10000);
}

void servoclose()
{
    stop_servo_process();
    char path_close[256];
    snprintf(path_close, sizeof(path_close), "%s/pwm0/enable", PWM_CHIP);

    FILE *fp4 = fopen(path_close, "w");
    if (fp4 == NULL)
    {
        perror("Failed to stop");
        exit(1);
    }
    fprintf(fp4, "%d", 0);
    fclose(fp4);
}

void send_light_data(int client_fd)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        FILE *fp = popen("/root/light_elf2", "r");
        if (!fp)
        {
            perror("popen failed");
            _exit(1);
        }
        else
        {
            printf("light_elf2 started successfully\n");
        }

        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp))
        {
            ssize_t sent = send(client_fd, buffer, strlen(buffer), MSG_NOSIGNAL);
            if (sent < 0)
            {
                perror("Failed to send light data");
                break;
            }
            usleep(100000);
        }

        pclose(fp);
        _exit(0);
    }
    else if (pid > 0)
    {
        add_child_process(pid, client_fd);
    }
    else
    {
        perror("fork failed");
    }
}

void detect(int client_fd)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        if (chdir("/bi_cam/rknn_yolov8_camera_demo") == -1)
        {
            perror("Failed to change directory");
            _exit(1);
        }

        execl("./rknn_yolov8_camera_demo", "./rknn_yolov8_camera_demo", "./model/yolov8.rknn", NULL);
        perror("Failed to execute rknn_yolov8_camera_demo");
        _exit(1);
    }
    else if (pid > 0)
    {
        add_child_process(pid, client_fd);
    }
    else
    {
        perror("fork failed");
    }
}


////////////////////////////////////////////////////////////主程序/////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction(SIGCHLD) failed");
        exit(1);
    }

    int iSocketServer;
    int iSocketClient;
    struct sockaddr_in tSocketServerAddr;
    struct sockaddr_in tSocketClientAddr;
    int iRet;
    int iAddrLen;
    int iRecvLen;
    unsigned char ucRecvBuf[1000];

    signal(SIGCHLD, SIG_IGN);

    iSocketServer = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == iSocketServer)
    {
        printf("socket error!\n");
        return -1;
    }

    tSocketServerAddr.sin_family = AF_INET;
    tSocketServerAddr.sin_port = htons(SERVER_PORT);
    tSocketServerAddr.sin_addr.s_addr = INADDR_ANY;
    memset(tSocketServerAddr.sin_zero, 0, 8);

    iRet = bind(iSocketServer, (const struct sockaddr *)&tSocketServerAddr, sizeof(struct sockaddr));
    if (-1 == iRet)
    {
        printf("bind error!\n");
        return -1;
    }

    iRet = listen(iSocketServer, BACKLOG);
    if (-1 == iRet)
    {
        printf("listen error!\n");
        return -1;
    }

    while (1)
    {
        iAddrLen = sizeof(struct sockaddr);
        iSocketClient = accept(iSocketServer, (struct sockaddr *)&tSocketClientAddr, &iAddrLen);
        if (-1 != iSocketClient)
        {
            printf("Connection from client: %s\n", inet_ntoa(tSocketClientAddr.sin_addr));

            while (1)
            {
                memset(ucRecvBuf, 0, sizeof(ucRecvBuf));
                iRecvLen = recv(iSocketClient, ucRecvBuf, sizeof(ucRecvBuf) - 1, 0);
                ucRecvBuf[iRecvLen] = '\0';
                if (iRecvLen <= 0)
                {
                    if (iRecvLen == 0)
                    {
                        printf("Client %s:%d disconnected\n",
                               inet_ntoa(tSocketClientAddr.sin_addr),
                               ntohs(tSocketClientAddr.sin_port));
                    }
                    else
                    {
                        perror("recv error");
                    }

                    printf("Cleaning up client resources...\n");
                    stop_child_processes(iSocketClient);

                    if (close(iSocketClient) == -1)
                    {
                        perror("close socket error");
                    }

                    break;
                }

                else if (strncmp(ucRecvBuf, "stopall", 7) == 0)
                {
                    stop_child_processes(iSocketClient);
                    stop_servo_process();
                    char *response = "All child processes stopped\n";
                    send(iSocketClient, response, strlen(response), 0);
                }
                else if (strncmp(ucRecvBuf, "servo90", 7) == 0)
                {
                    printf("Turning servo to 90 degrees\n");
                    servo90();
                    char *response = "Servo turned to 90 degrees\n";
                    send(iSocketClient, response, strlen(response), 0);
                }
                else if (strncmp(ucRecvBuf, "servoclose", 10) == 0)
                {
                    printf("Stopping servo\n");
                    servoclose();
                    char *response = "Servo stopped\n";
                    send(iSocketClient, response, strlen(response), 0);
                }
                else if (strncmp(ucRecvBuf, "patrol", 6) == 0)
                {
                    printf("Starting patrol mode...\n");
                    stop_servo_process();

                    pid_t pid = fork();
                    if (pid == 0)
                    {
                        execl("/root/serveo_inspection_elf2", "serveo_inspection_elf2", NULL);
                        perror("exec failed");
                        _exit(1);
                    }
                    else if (pid > 0)
                    {
                        g_servo_pid = pid;
                        add_child_process(pid, iSocketClient);
                        char *response = "Patrol mode started\n";
                        send(iSocketClient, response, strlen(response), 0);
                    }
                    else
                    {
                        perror("fork failed");
                        char *response = "Failed to start patrol mode\n";
                        send(iSocketClient, response, strlen(response), 0);
                    }
                }
                else if (strncmp(ucRecvBuf, "get_light", 9) == 0)
                {
                    printf("Sending light data\n");
                    char *response = "Sending light data\n";
                    send(iSocketClient, response, strlen(response), 0);
                    send_light_data(iSocketClient);
                }
                else if (strncmp(ucRecvBuf, "detect", 6) == 0)
                {
                    printf("Starting detection\n");
                    servoclose();
                    char *response = "Detection started\n";
                    send(iSocketClient, response, strlen(response), 0);
                    detect(iSocketClient);
                }

                else if (strncmp(ucRecvBuf, "stoppatrol", 10) == 0)
                {                  
                    stop_specific_process(iSocketClient, "serveo_inspection_elf2"); // 直接传递二进制名称
                    printf("Stopping patrol mode\n");
                    char *response = "Patrol mode stopped\n";
                    send(iSocketClient, response, strlen(response), 0);
                }

                else if (strncmp(ucRecvBuf, "stoplight", 9) == 0)
                {
                    system("pkill light_elf2");
                    printf("Stopping light detection\n");
                    char *response = "Light detection stopped\n";
                    send(iSocketClient, response, strlen(response), 0);
                }


                else if (strncmp(ucRecvBuf, "stopdetect", 10) == 0)
                {
                    stop_specific_process(iSocketClient, "rknn_yolov8_camera_demo"); // 直接传递二进制名称
                    printf("Stopping object detection\n");
                    char *response = "Object detection stopped\n";
                    send(iSocketClient, response, strlen(response), 0);
                }

                else
                {
                    printf("Received message from client: %s\n", ucRecvBuf);
                }
            }
        }
    }

    close(iSocketServer);
    return 0;
}