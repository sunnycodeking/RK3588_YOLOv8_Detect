#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#define SHM_KEY 0x1234 // 与接收端一致的key
#define SHM_SIZE sizeof(shm_data_t)
#define MIN_DUTY_NS 1100000 // 最小占空比
#define MAX_DUTY_NS 3000000 // 最大占空比
#define STEP_SIZE 100000    // 步长
#define INTERVAL_US 300000  // 发送间隔(0.3秒)

// 共享内存数据结构(与接收端一致)
typedef struct
{
    int duty_cycle;
    time_t last_update;
} shm_data_t;

volatile int running = 1;
int shmid = -1;
shm_data_t *shared_data = NULL;



void handle_signal(int sig)
{
    printf("\n[Sender] Stopping gracefully...\n");
    running = 0;
}


void init_shared_memory()
{
    // 创建共享内存(如果不存在)
    if ((shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666)) == -1)
    {
        perror("shmget failed");
        exit(EXIT_FAILURE);
    }



    if ((shared_data = (shm_data_t *)shmat(shmid, NULL, 0)) == (void *)-1)
    {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }

    // 初始化数据
    shared_data->duty_cycle = MIN_DUTY_NS; 
    shared_data->last_update = time(NULL);
}

void cleanup()
{
    if (shared_data)
    {
        shmdt(shared_data);
    }

    if (shmid != -1)
    {
        shmctl(shmid, IPC_RMID, NULL);
        printf("[Sender] Shared memory removed\n");
    }
}

int main()
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    init_shared_memory();
    printf("[Sender] Started. Sending duty cycles from %d to %d ns\n",
           MIN_DUTY_NS, MAX_DUTY_NS);

    int direction = 1; // 1表示增加，-1表示减少
    int current_duty = MIN_DUTY_NS;

    while (running)
    {
        // 更新占空比(锯齿波模式)
        current_duty += direction * STEP_SIZE;

        // 检查边界
        if (current_duty >= MAX_DUTY_NS)
        {
            current_duty = MAX_DUTY_NS;
            direction = -1;
        }
        else if (current_duty <= MIN_DUTY_NS)
        {
            current_duty = MIN_DUTY_NS;
            direction = 1;
        }

        // 写入共享内存
        shared_data->duty_cycle = current_duty;
        shared_data->last_update = time(NULL);

        printf("[Sender] Sent duty cycle: %d ns\n", current_duty);

        // 等待指定间隔
        usleep(INTERVAL_US);
    }

    cleanup();
    return 0;
}