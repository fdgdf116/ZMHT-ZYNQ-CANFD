# 原镜像 + 独立 CANFD 程序

这个目录用于“不把程序固化进 `image.ub`”的方式：

1. 板子启动原镜像（`zmht-init` 已配置 `can0~can5` 和驱动）。
2. 在开发机上交叉编译 `canfd_server` / `canfd_test`。
3. 用 scp 或 SD 卡把二进制拷到板子。
4. 在板子上手工运行，程序通过 SocketCAN 调用内核 `xilinx_can` 驱动发送。

## 编译

```sh
cd /home/wang/ZMHT-Project/ZMHT-ZYNQ-CANFD
./standalone/build.sh
```

产物：

```text
build-standalone/canfd_server   # TCP 服务：9009/9010/9012
standalone/canfd_test           # 直接 SocketCAN 收发测试工具
```

两个都是 32 位 ARM 程序。`canfd_server` 的 `libstdc++/libgcc` 已静态链接，
原镜像只需要 `libc/libpthread/libm`。

## 部署

```sh
./standalone/deploy.sh 192.168.8.24 /home/root
```

`/home/root` 在 initramfs 里，重启会丢。要持久化，放到 SD 卡 FAT：

```sh
./standalone/deploy.sh 192.168.8.24 /run/media/mmcblk0p1
```

也可以手工：

```sh
scp -O -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  build-standalone/canfd_server root@192.168.8.24:/home/root/
```

## 先验证驱动

板子上先确认接口：

```sh
ip -br link
ip -details link show can0
```

如果 `can0` 还是 down，按交接文档配置：

```sh
ip link set can0 down
ip link set can0 type can bitrate 500000 sample-point 0.875 \
    dbitrate 2000000 dsample-point 0.750 fd on loopback off
ip link set can0 up
```

直接发一帧 64 字节 CAN FD：

```sh
/home/root/canfd_test tx can0 0x123 \
  00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f \
  10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f \
  20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f \
  30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f --fd
```

ZCANpro 能看到这帧，说明原镜像的 `xilinx_can` 驱动和收发器链路正常。
接收测试：

```sh
/home/root/canfd_test rx can0
```

## 运行 TCP 服务

```sh
chmod +x /home/root/canfd_server
/home/root/canfd_server
```

正常日志：

```text
[CANFD] hardware layer ready (SocketCAN)
[CANFD] ringbuffer ready channels=6 ...
[CANFD 9010] server ready
[CANFD] port 9009 ready
[CANFD] port 9012 ready
```

上位机流程：

1. 先连 9010（应答和上行都从这里出）。
2. 9009 发 0x0101 初始化包。
3. 9009 发 0x0103，状态 0xAA 打开通道。
4. 9012 发 0x0001 + 112 字节 CANFD 包。
5. 9010 收 0x0080，`States=1`；CAN 总线上出现帧。

程序内部调用链：

```text
9012 -> network_main.cpp -> protocol.cpp canfd_data_normal
     -> canfd_hw.cpp canfd_hw_submit_frame
     -> PF_CAN raw socket -> xilinx_can -> CANFD IP -> 总线
```

## 板端代码直接调用

如果不想经过 TCP，自己的板端代码可以直接使用
[`src/canfd/canfd_service.h`](../src/canfd/canfd_service.h)：

```cpp
#include "canfd_service.h"

static void on_rx(const canfd_service_frame_t* frame, void* user)
{
    (void)user;
    // frame->channel / can_id / data / length / fd / brs
}

int main()
{
    canfd_service_set_rx_callback(on_rx, nullptr);
    if (canfd_service_start() != 0) {
        return 1;
    }

    uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t state = 0;
    canfd_service_send(0, 0x123, data, sizeof(data),
                       1 /*fd*/, 0 /*brs*/, 0 /*ext*/, 0 /*remote*/, &state);

    for (;;) {
        sleep(1);
    }
}
```

`canfd_service_start()` 内部会同时启动：

- 9009 命令端口
- 9012 数据端口
- 9010 应答/上行端口
- can0~can5 的 SocketCAN 接收线程

所以 TCP 上位机和板内代码可以同时使用同一套收发接口。

## 注意

- 不要加载 `zmuav_axican.ko`，CAN 由内核 `xilinx_can` 驱动接管。
- 板子上要 root 运行（线程用 `SCHED_FIFO`，并执行 `ip link`）。
- BRS 默认关闭；交接文档记录 FPGA/驱动组合下 BRS 会进 BUS-OFF。
- 这套程序完全不改 `image.ub`；原镜像恢复后的 sha256 为
  `3a158587cbb4f22d29fdc0691696d785727b5bd481ca1b72dbac0cf9c7c57643`。
