#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <fcntl.h> // 用于 open()

#define SERVER_PORT 8888
#define VIDEO_PORT 5000

bool is_streaming = false;
pid_t gst_pid = -1;

// 函数声明（解决未定义问题）
void handle_signal(int sig);
void stop_video_stream(void);
void start_video_stream(const char *client_ip);

// 信号处理函数定义
void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        stop_video_stream();
        exit(0);
    }
}

void stop_video_stream()
{
    if (gst_pid > 0)
    {
        printf("Stopping video stream (PID: %d)...\n", gst_pid);

        // 杀死整个进程组
        killpg(gst_pid, SIGTERM);

        // 等待进程结束
        int status;
        waitpid(gst_pid, &status, 0);

        gst_pid = -1;
        is_streaming = false;
        printf("Video stream stopped successfully\n");
    }
}

void start_video_stream(const char *client_ip)
{
    if (is_streaming)
    {
        stop_video_stream();
    }

    // 清理 IP 地址
    char cleaned_ip[16];
    strncpy(cleaned_ip, client_ip, 15);
    cleaned_ip[15] = '\0';
    cleaned_ip[strcspn(cleaned_ip, "\n\r ")] = '\0';

    pid_t pid = fork();
    if (pid == 0)
    {
        setpgid(0, 0); // 创建新的进程组

        // 重定向所有输出到/dev/null
        int null_fd = open("/dev/null", O_WRONLY);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);

        // 动态构造 GStreamer 参数
        char host_param[64];
        char port_param[64];
        snprintf(host_param, sizeof(host_param), "host=%s", cleaned_ip);
        snprintf(port_param, sizeof(port_param), "port=%d", VIDEO_PORT);

        execlp("gst-launch-1.0", "gst-launch-1.0",
               "v4l2src", "device=/dev/video11",
               "!", "video/x-raw,format=NV12,width=320,height=240,framerate=15/1",
               "!", "videoconvert",
               "!", "jpegenc", "quality=50",
               "!", "rtpjpegpay", "pt=26",
               "!", "udpsink", host_param, port_param,
               NULL);

        perror("execlp failed");
        exit(1);
    }
    else if (pid > 0)
    {
        gst_pid = pid;
        is_streaming = true;
    }
    else
    {
        perror("fork failed");
    }
}

int main(int argc, char **argv)
{
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    char buffer[1024];
    int bytes_read;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. 创建 TCP Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);//文件描述符的本质是进程索引，监控server_fd就是监控新的连接进程
    if (server_fd == -1)
    {
        perror("socket");
        return -1;
    }

    // 2. 绑定地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    memset(server_addr.sin_zero, 0, 8);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(server_fd);
        return -1;
    }

    // 在 bind() 之后、listen() 之前：
    printf("Checking WiFi connection...\n");
    if (system("ping -c 1 -W 1 `ip route | awk '/default/ {print $3}'` > /dev/null 2>&1") != 0)
    {
        printf("WiFi not connected or no route to gateway! Check STA mode.\n");
        close(server_fd);
        return -1;
    }
    printf("Server started, waiting for commands...\n");


    // 3. 监听连接
    if (listen(server_fd, 5) == -1)
    {
        perror("listen");
        close(server_fd);
        return -1;
    }

    fd_set readfds;
    int max_fd;
    int client_fds[FD_SETSIZE]; // 存储所有客户端fd
    int num_clients = 0;

    // 初始化client_fds数组
    for (int i = 0; i < FD_SETSIZE; i++)
    {
        client_fds[i] = -1;
    }

        while (1)
        {
           
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds); 
            max_fd = server_fd;

          
            ​ for (int i = 0; i < FD_SETSIZE; i++)
            {
                if (client_fds[i] > 0)
                {
                    FD_SET(client_fds[i], &readfds);
                    if (client_fds[i] > max_fd)
                    {
                        max_fd = client_fds[i];
                    }
                }
            }

            struct timeval tv = {1, 0};
            int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);

            if (ret == -1)
            {
                perror("select");
                // 检查是否有无效的fd导致错误
                for (int i = 0; i < FD_SETSIZE; i++)
                {
                    if (client_fds[i] > 0 && fcntl(client_fds[i], F_GETFL) == -1)
                    {
                        close(client_fds[i]);
                        client_fds[i] = -1;
                    }
                }
                continue;
            }

            // 检查服务器套接字是否有新连接
            if (FD_ISSET(server_fd, &readfds))
            {
                client_len = sizeof(client_addr);
                client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd == -1)
                {
                    perror("accept");
                    continue;
                }

                // 将新客户端添加到数组
                for (int i = 0; i < FD_SETSIZE; i++)
                {
                    if (client_fds[i] == -1)
                    {
                        client_fds[i] = client_fd;
                        printf("New client connected: %s\n", inet_ntoa(client_addr.sin_addr));
                        break;
                    }
                }
            }

            // 检查所有客户端套接字是否有数据
            for (int i = 0; i < FD_SETSIZE; i++)
            {
                if (client_fds[i] > 0 && FD_ISSET(client_fds[i], &readfds))
                {
                    bytes_read = recv(client_fds[i], buffer, sizeof(buffer) - 1, 0);
                    if (bytes_read <= 0)
                    {
                        // 客户端断开连接
                        printf("Client disconnected\n");
                        close(client_fds[i]);
                        client_fds[i] = -1; // 必须设置为-1，避免下次select使用无效fd
                        continue;
                    }

                    buffer[bytes_read] = '\0';
                    printf("Received command: %s\n", buffer);

                    // 6. 处理命令
                    if (strncmp(buffer, "start", 5) == 0)
                    {
                        printf("Starting video stream to %s...\n", inet_ntoa(client_addr.sin_addr));
                        start_video_stream(inet_ntoa(client_addr.sin_addr));
                        send(client_fd, "Video streaming started", 23, 0);
                    }
                    // 服务器代码片段
                    else if (strncmp(buffer, "stop", 4) == 0)
                    {
                        printf("Stopping video stream...\n");
                        stop_video_stream();
                        const char *response = "server Video streaming stopped";
                        send(client_fd, response, strlen(response), 0);
                    }
                    else
                    {
                        send(client_fd, "Unknown command", 15, 0);
                    }

                    
                }
            }
        }

        close(server_fd);
        return 0;
}
