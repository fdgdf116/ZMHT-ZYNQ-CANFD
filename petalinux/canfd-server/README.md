# canfd-server PetaLinux 集成

把当前工作区的 C++ CANFD-over-TCP 服务固化进 PetaLinux rootfs。
recipe 的 `files/canfd-src.tar.gz` 就是当前工作区 `src/` 目录树的快照，
保留 `src/canfd`、`src/network`、`src/hardware` 结构。

## 从当前目录同步

```sh
./petalinux/sync-to-petalinux.sh
```

脚本会重新打包当前 `src/`，安装到：

```text
CanFD/zmht_canfd/project-spec/meta-user/recipes-apps/canfd-server/
```

## 镜像 recipe

PetaLinux 2019.1 实际构建的是 `petalinux-user-image`，不是
`petalinux-image-full`。因此在 `meta-user/recipes-core/images/` 下需要：

```text
petalinux-user-image.bbappend:
IMAGE_INSTALL_append = " canfd-server"
```

## 编译

进入容器后：

```sh
source /opt/petalinux/2019.1/settings.sh
cd /workspace/sdk/CanFD/zmht_canfd
petalinux-build -c canfd-server
petalinux-build
```

新 `images/linux/image.ub` 会包含：

- `/usr/bin/canfd_server`
- `/etc/init.d/canfd-server`
- 开机顺序：S99，日志在 `/var/log/canfd_server.log`

## 运行

板子上：

```sh
/etc/init.d/canfd-server start
ps | grep canfd_server
netstat -ltn | grep -E '9009|9010|9012'
```

上位机先连 9010，再连 9009/9012。发送顺序：

1. 9009 发 0x0101 初始化配置
2. 9009 发 0x0103（0xAA）打开通道
3. 9012 发 0x0001 数据
4. 9010 收 0x80 反馈

## 注意

- 服务以 root 运行，内部线程使用 `SCHED_FIFO`，并调用 `ip link`。
- `zmht-init` 已经会配置 can0~can5；本服务的 0x0101 会按上位机参数重新配置。
- 端口 9009/9010/9012 只能各有一个客户端。
