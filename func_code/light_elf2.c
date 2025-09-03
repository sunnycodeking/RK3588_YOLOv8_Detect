#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>

// GY-30 (BH1750) 默认参数
#define I2C_DEV "/dev/i2c-4"
#define BH1750_ADDR 0x23
#define POWER_DOWN 0x00
#define POWER_ON 0x01
#define RESET 0x07
#define ONETIME_H_RES_MODE 0x20  // 单次高精度模式

int i2c_init(const char* device, int addr) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Failed to open I2C device %s: %s\n",
            device, strerror(errno));
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        fprintf(stderr, "Failed to set I2C address 0x%02x: %s\n",
            addr, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int bh1750_write(int fd, uint8_t cmd) {
    if (write(fd, &cmd, 1) != 1) {
        fprintf(stderr, "Failed to write command 0x%02x: %s\n",
            cmd, strerror(errno));
        return -1;
    }
    return 0;
}

int bh1750_read(int fd, uint16_t* lux) {
    uint8_t buf[2];
    if (read(fd, buf, 2) != 2) {
        fprintf(stderr, "Failed to read data: %s\n", strerror(errno));
        return -1;
    }
    *lux = (buf[0] << 8) | buf[1];
    *lux = (int)(*lux / 1.2);  // 转换为lux值
    return 0;
}

int main() {
    int fd;
    uint16_t lux;
    time_t timestamp;

    // 1. 初始化I2C
    if ((fd = i2c_init(I2C_DEV, BH1750_ADDR)) < 0) {
        exit(EXIT_FAILURE);
    }

    // 2. 发送上电指令
    if (bh1750_write(fd, POWER_ON) < 0) {
        close(fd);
        exit(EXIT_FAILURE);
    }

    // 3. 发送复位指令
    if (bh1750_write(fd, RESET) < 0) {
        close(fd);
        exit(EXIT_FAILURE);
    }
    usleep(10000);  // 复位后短暂延迟

    

    while (1) {
        // 5. 触发单次测量
        if (bh1750_write(fd, ONETIME_H_RES_MODE) < 0) {
            close(fd);
            exit(EXIT_FAILURE);
        }

        // 6. 等待测量完成（120ms + 余量）
        usleep(180000);

        // 7. 读取数据
        if (bh1750_read(fd, &lux) < 0) {
            close(fd);
            exit(EXIT_FAILURE);
        }

       

        printf("Current illuminance: %d lux\n", lux);
        fflush(stdout);  // 确保立即输出

        // 10. 等待3秒
        sleep(3);
    }

    // 理论上不会执行到这里
    bh1750_write(fd, POWER_DOWN);
    close(fd);
    return EXIT_SUCCESS;
}