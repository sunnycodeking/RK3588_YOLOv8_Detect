#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>  // 用于 open() 和 O_RDWR
#include <unistd.h> // 用于 dup2() 和 close()

#define SERVER_PORT 8888
#define VIDEO_PORT 5000

bool is_receiving = false;
pid_t gst_pid = -1;

void stop_video_receiver()
{
    if (gst_pid > 0)
    {
        // 先尝试优雅终止（SIGTERM）
        killpg(gst_pid, SIGTERM);

        // 等待最多1秒
        for (int i = 0; i < 10; i++)
        {
            if (waitpid(gst_pid, NULL, WNOHANG) > 0)
            {
                break; // 进程已退出
            }
            usleep(100000); // 100ms
        }

        // 若未终止，强制杀死（SIGKILL）
        killpg(gst_pid, SIGKILL);
        waitpid(gst_pid, NULL, 0);

        gst_pid = -1;
    }
    is_receiving = false;
}

void start_video_receiver()
{
    if (is_receiving)
    {
        stop_video_receiver();
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        // 子进程：创建新进程组
        setpgid(0, 0);

        // 重定向输出到 /dev/null
        int null_fd = open("/dev/null", O_RDWR);
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);

        // 启动 GStreamer
        execlp("gst-launch-1.0", "gst-launch-1.0",
               "udpsrc", "port=5000",
               "!", "application/x-rtp,media=video,payload=26",
               "!", "rtpjpegdepay",
               "!", "jpegdec",
               "!", "videoconvert",
               "!", "autovideosink",
               NULL);
        exit(1);
    }
    else if (pid > 0)
    {
        gst_pid = pid;
        is_receiving = true;
    }
    else
    {
        perror("fork failed");
    }
}

void send_command(int sock, const char *cmd)
{
    // 发送命令
    send(sock, cmd, strlen(cmd), 0);

    // 设置接收超时（1秒）
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 接收响应
    char response[100];
    int len = recv(sock, response, sizeof(response) - 1, 0);
    if (len > 0)
    {
        response[len] = '\0';
        printf("Server response: %s\n", response);
    }
    else
    {
        printf("No response from server (timeout)\n");
    }
}

int main(int argc, char **argv)
{
    int sock;
    struct sockaddr_in server_addr;

    if (argc != 2)
    {
        printf("Usage: %s <server_ip>\n", argv[0]);
        return -1;
    }

    // 1. 创建 TCP Socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1)
    {
        perror("socket");
        return -1;
    }

    // 2. 连接服务器
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_aton(argv[1], &server_addr.sin_addr) == 0)
    {
        printf("Invalid server IP\n");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("connect");
        close(sock);
        return -1;
    }
    else
    {
        printf("connect successfuly,please input command!\n");
    }

    
    while (1)
    {
        
        char input[100];
        if (fgets(input, sizeof(input), stdin))
        {
            input[strcspn(input, "\n")] = '\0';

            if (strcmp(input, "start") == 0)
            {
                send_command(sock, "start");
                start_video_receiver();
            }
            else if (strcmp(input, "stop") == 0)
            {
                printf(" Sending 'stop' command to server...\n");
                send_command(sock, "stop");
                printf("Calling stop_video_receiver()...\n");
                stop_video_receiver();
                printf("Video receiver stopped.\n");
            }
            else if (strcmp(input, "exit") == 0)
            {
                stop_video_receiver();
                break;
            }
            else
            {
                printf("Unknown command\n");
            }
        }
    }

    close(sock);
    return 0;
}