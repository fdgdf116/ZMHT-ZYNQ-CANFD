#ifndef UTILITY_COMMON_H_
#define UTILITY_COMMON_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <chrono>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

#ifndef NULL
#define NULL 0
#endif

#ifndef FAIL
#define FAIL -1
#endif

#ifndef OK
#define OK 0
#endif

#define ASSERT assert

#define MAX_PATH_LENGTH	256

#define DOWNLOAD_PATH "/run/media/mmcblk0p1/"

#define CPU_NUMS 4

#endif /* UTILITY_COMMON_H_ */
