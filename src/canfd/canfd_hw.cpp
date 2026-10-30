#include "hw.h"

#include "canfd_codec.h"
#include "protocol.h"
#include "sync_send.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {

static uint32_t le32_read(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static void le32_write(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

const int kChannelCount = CANFD_CHANNEL_COUNT;

struct ChannelHw {
    bool configured;
    bool stub;
    bool running;
    int fd;
    uint32_t baud;
    uint32_t data_baud;
    uint8_t mode;
    uint8_t auto_send;
    uint8_t brs_en;
    uint32_t packet_interval;
    uint64_t last_tx_us;
    uint32_t ch_count;
    uint32_t err_count;
    pthread_t rx_thread;
    volatile bool rx_stop;
    bool rx_started;
};

ChannelHw g_ch[kChannelCount];
pthread_mutex_t g_count_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_total_count = 0;
int g_any_interface = 0;
bool g_started = false;
pthread_mutex_t g_rx_cb_lock = PTHREAD_MUTEX_INITIALIZER;
canfd_hw_rx_cb g_rx_cb = nullptr;
void* g_rx_user = nullptr;

void ensure_started();

uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

void write_be64(uint8_t* dest, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        dest[i] = static_cast<uint8_t>((value >> (56 - 8 * i)) & 0xFF);
    }
}

void reverse_data_words(uint8_t* data, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i += 4) {
        const uint8_t b0 = data[i];
        const uint8_t b1 = data[i + 1];
        data[i] = data[i + 3];
        data[i + 1] = data[i + 2];
        data[i + 2] = b1;
        data[i + 3] = b0;
    }
}

void dump_hex(const char* tag, const uint8_t* data, size_t len)
{
    printf("%s (%u bytes):", tag, static_cast<unsigned>(len));
    for (size_t i = 0; i < len; ++i) {
        if ((i % 16) == 0) printf("\n  ");
        printf("%02X ", data[i]);
    }
    printf("\n");
}

const char* iface_name(uint8_t channel, char* buf, size_t size)
{
    snprintf(buf, size, "can%u", static_cast<unsigned>(channel));
    return buf;
}

bool iface_present(uint8_t channel)
{
    char iface[IFNAMSIZ];
    char path[64];
    iface_name(channel, iface, sizeof(iface));
    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);
    return access(path, F_OK) == 0;
}

uint32_t next_total_count()
{
    pthread_mutex_lock(&g_count_lock);
    const uint32_t value = ++g_total_count;
    pthread_mutex_unlock(&g_count_lock);
    return value;
}

int configure_interface(uint8_t channel, const ChannelHw& cfg)
{
    char iface[IFNAMSIZ];
    char cmd[512];
    iface_name(channel, iface, sizeof(iface));

    const uint32_t dbitrate = cfg.data_baud ? cfg.data_baud : cfg.baud;
    const char* loopback = (cfg.mode == 0x04) ? " loopback on" : "";

    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", iface);
    if (system(cmd) != 0) {
        printf("[CANFD] %s down failed\n", iface);
    }

    /* Match the ground-station timing first (80% on both segments), then
     * fall back to the previously board-tested points and finally let the
     * driver calculate its own timing. */
    snprintf(cmd, sizeof(cmd),
             "ip link set %s type can bitrate %u sample-point 0.800 "
             "dbitrate %u dsample-point 0.800 fd on%s",
             iface, cfg.baud, dbitrate, loopback);
    printf("[CANFD] configure: %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        snprintf(cmd, sizeof(cmd),
                 "ip link set %s type can bitrate %u sample-point 0.875 "
                 "dbitrate %u dsample-point 0.750 fd on%s",
                 iface, cfg.baud, dbitrate, loopback);
        printf("[CANFD] configure fallback: %s\n", cmd);
        rc = system(cmd);
    }
    if (rc != 0) {
        snprintf(cmd, sizeof(cmd),
                 "ip link set %s type can bitrate %u dbitrate %u fd on%s",
                 iface, cfg.baud, dbitrate, loopback);
        printf("[CANFD] configure fallback: %s\n", cmd);
        rc = system(cmd);
    }
    if (rc != 0) {
        printf("[CANFD] %s bitrate config failed (bitrate=%u dbitrate=%u)\n",
               iface, cfg.baud, dbitrate);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", iface);
    if (system(cmd) != 0) {
        printf("[CANFD] %s up failed\n", iface);
        return -1;
    }
    printf("[CANFD] %s configured bitrate=%u dbitrate=%u fd=%s brs=%u mode=0x%02X\n",
           iface, cfg.baud, dbitrate, dbitrate ? "on" : "off",
           static_cast<unsigned>(cfg.brs_en), static_cast<unsigned>(cfg.mode));
    return 0;
}

void fill_rx_packet(uint8_t channel, const canfd_message_t& msg, uint8_t packet[112])
{
    memset(packet, 0, CANFD_FRAME_SIZE);
    /* Inner protocol words are little-endian on the wire: 0x499602D2
     * appears as D2 02 96 49 in the capture. */
    le32_write(packet, CANFD_INNER_HEAD);
    packet[4] = 1;                                  /* device -> host */
    packet[5] = static_cast<uint8_t>(UPLINK_INDEX_RX);
    packet[6] = CANFD_FRAME_SIZE;
    packet[7] = channel;
    le32_write(packet + 8, next_total_count());
    le32_write(packet + 12, ++g_ch[channel].ch_count);
    /* CAN bus data words are stored in the opposite order from the upper
     * computer's little-endian words.  Convert only the payload data; the
     * inner header, IDR, DRCR and counters keep their wire format. */
    canfd_message_t upper_msg = msg;
    reverse_data_words(upper_msg.data, upper_msg.len);
    canfd_packet_encode_message(packet, &upper_msg);
    packet[96] = 0;                                 /* States: 0 = receive ok */
    packet[97] = 0;
    write_be64(packet + 100, now_us());
    le32_write(packet + 108, canfd_xor(packet, 108));
}

void* rx_thread_main(void* arg)
{
    ChannelHw* ch = static_cast<ChannelHw*>(arg);
    const uint8_t channel = static_cast<uint8_t>(ch - g_ch);
    uint8_t buf[CANFD_MTU];

    while (!ch->rx_stop) {
        struct pollfd pfd;
        pfd.fd = ch->fd;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, 200);
        if (ready <= 0) {
            continue;
        }
        const int n = static_cast<int>(read(ch->fd, buf, sizeof(buf)));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (ch->rx_stop) {
                break;
            }
            continue;
        }

        canfd_message_t msg;
        memset(&msg, 0, sizeof(msg));

        if (n == CANFD_MTU) {
            struct canfd_frame* frame = reinterpret_cast<struct canfd_frame*>(buf);
            msg.fd = 1;
            msg.len = frame->len > CANFD_MAX_DATA_LEN ? CANFD_MAX_DATA_LEN : frame->len;
            msg.brs = (frame->flags & CANFD_BRS) ? 1 : 0;
            msg.esi = (frame->flags & CANFD_ESI) ? 1 : 0;
            if (frame->can_id & CAN_EFF_FLAG) {
                msg.eff = 1;
                msg.can_id = frame->can_id & CAN_EFF_MASK;
            } else {
                msg.can_id = frame->can_id & CAN_SFF_MASK;
            }
            memcpy(msg.data, frame->data, msg.len);
        } else if (n == CAN_MTU) {
            struct can_frame* frame = reinterpret_cast<struct can_frame*>(buf);
            if (frame->can_id & CAN_ERR_FLAG) {
                ++ch->err_count;
                continue;   /* error frames are counted, not forwarded */
            }
            msg.len = frame->can_dlc > 8 ? 8 : frame->can_dlc;
            msg.rtr = (frame->can_id & CAN_RTR_FLAG) ? 1 : 0;
            if (frame->can_id & CAN_EFF_FLAG) {
                msg.eff = 1;
                msg.can_id = frame->can_id & CAN_EFF_MASK;
            } else {
                msg.can_id = frame->can_id & CAN_SFF_MASK;
            }
            memcpy(msg.data, frame->data, msg.len);
        } else {
            continue;
        }

        msg.timestamp = static_cast<uint16_t>(now_us() & 0xFFFFU);
        canfd_hw_rx_cb cb;
        void* user;
        pthread_mutex_lock(&g_rx_cb_lock);
        cb = g_rx_cb;
        user = g_rx_user;
        pthread_mutex_unlock(&g_rx_cb_lock);
        if (cb != nullptr) {
            cb(channel, &msg, user);
        }
        printf("[CANFD RX] ch=%u id=0x%X len=%u %s%s\n",
               channel, msg.can_id, msg.len, msg.fd ? "FD " : "CAN ",
               msg.brs ? "BRS" : "");
        dump_hex("[CANFD RX DATA]", msg.data, msg.len);
        uint8_t packet[CANFD_FRAME_SIZE];
        fill_rx_packet(channel, msg, packet);
        /* The encoded 112-byte uplink packet is protocol transport data and
         * is intentionally not dumped here.  Keep the decoded CAN frame log
         * above so received payloads remain visible without flooding the
         * console with duplicate packet bytes. */
        (void)canfd_send_uplink(UPLINK_INDEX_RX, packet, CANFD_FRAME_SIZE);
    }
    return nullptr;
}

void stop_channel(ChannelHw* ch)
{
    if (ch->rx_started) {
        ch->rx_stop = true;
        if (ch->fd >= 0) {
            shutdown(ch->fd, SHUT_RD);
        }
        pthread_join(ch->rx_thread, nullptr);
        ch->rx_started = false;
    }
    if (ch->fd >= 0) {
        close(ch->fd);
        ch->fd = -1;
    }
    ch->running = false;
    ch->rx_stop = false;
}

}  // namespace

extern "C" int canfd_hw_start(void)
{
    memset(g_ch, 0, sizeof(g_ch));
    g_any_interface = 0;
    for (int i = 0; i < kChannelCount; ++i) {
        g_ch[i].fd = -1;
        if (iface_present(static_cast<uint8_t>(i))) {
            g_any_interface = 1;
        } else {
            g_ch[i].stub = true;
        }
    }
    g_started = true;
    printf("[CANFD] hardware layer ready (%s)\n",
           g_any_interface ? "SocketCAN" : "stub, no canX interface");
    return 0;
}

extern "C" void canfd_hw_stop(void)
{
    for (int i = 0; i < kChannelCount; ++i) {
        stop_channel(&g_ch[i]);
    }
}

extern "C" int canfd_hw_apply_config(uint8_t channel, const uint8_t* init_frame, uint32_t length)
{
    ensure_started();
    if (channel >= kChannelCount || init_frame == nullptr || length != CANFD_FRAME_SIZE) {
        return -1;
    }
    ChannelHw* ch = &g_ch[channel];
    ch->baud = le32_read(init_frame + 16);
    ch->mode = init_frame[20];
    ch->auto_send = init_frame[21];
    ch->packet_interval = le32_read(init_frame + 24);
    ch->data_baud = le32_read(init_frame + 28);
    ch->brs_en = init_frame[32];
    ch->configured = true;
    ch->last_tx_us = 0;

    if (ch->stub) {
        printf("[CANFD] can%u absent, stub config bitrate=%u dbitrate=%u mode=0x%02X\n",
               static_cast<unsigned>(channel), ch->baud, ch->data_baud,
               static_cast<unsigned>(ch->mode));
        return 0;
    }

    if (ch->running) {
        stop_channel(ch);
    }
    return configure_interface(channel, *ch);
}

extern "C" int canfd_hw_set_run(uint8_t channel, uint32_t state)
{
    ensure_started();
    if (channel >= kChannelCount) {
        return -1;
    }
    ChannelHw* ch = &g_ch[channel];
    if (state == CANFD_RUN_CLOSE) {
        if (ch->stub) {
            printf("[CANFD] can%u absent, stub close\n", static_cast<unsigned>(channel));
            return 0;
        }
        stop_channel(ch);
        printf("[CANFD] can%u closed\n", static_cast<unsigned>(channel));
        return 0;
    }
    if (state != CANFD_RUN_OPEN) {
        return -1;
    }
    if (ch->stub) {
        printf("[CANFD] can%u absent, stub open\n", static_cast<unsigned>(channel));
        return 0;
    }
    if (!ch->configured) {
        printf("[CANFD] can%u not configured, refusing open\n", static_cast<unsigned>(channel));
        return -1;
    }
    if (ch->running) {
        return 0;
    }

    char iface[IFNAMSIZ];
    iface_name(channel, iface, sizeof(iface));
    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        printf("[CANFD] %s socket failed: %s\n", iface, strerror(errno));
        return -1;
    }
    int enable = 1;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) != 0) {
        printf("[CANFD] %s CAN_RAW_FD_FRAMES failed: %s\n", iface, strerror(errno));
        close(fd);
        return -1;
    }
    const can_err_mask_t err_mask = CAN_ERR_MASK;
    (void)setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
        printf("[CANFD] %s SIOCGIFINDEX failed: %s\n", iface, strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        printf("[CANFD] %s bind failed: %s\n", iface, strerror(errno));
        close(fd);
        return -1;
    }

    ch->fd = fd;
    ch->rx_stop = false;
    ch->running = true;
    if (pthread_create(&ch->rx_thread, nullptr, rx_thread_main, ch) != 0) {
        printf("[CANFD] %s rx thread failed\n", iface);
        close(fd);
        ch->fd = -1;
        ch->running = false;
        return -1;
    }
    ch->rx_started = true;
    printf("[CANFD] %s opened\n", iface);
    return 0;
}

extern "C" int canfd_hw_submit_frame(uint8_t channel, uint8_t type, const uint8_t* frame, uint32_t length)
{
    ensure_started();
    if (channel >= kChannelCount || frame == nullptr || length != CANFD_FRAME_SIZE) {
        return -1;
    }
    if (le32_read(frame) != CANFD_INNER_HEAD) {
        return -1;
    }
    ChannelHw* ch = &g_ch[channel];
    canfd_message_t msg;
    if (canfd_packet_decode_message(frame, &msg) != 0) {
        return -1;
    }
    /* BRS must be enabled per channel before a frame may switch bit rate. */
    if (msg.brs && !ch->brs_en) {
        printf("[CANFD] can%u reject BRS frame (BRS not enabled)\n",
               static_cast<unsigned>(channel));
        return -1;
    }
    if (ch->stub) {
        printf("[CANFD] can%u absent, stub tx type=0x%02X id=0x%X len=%u fd=%u brs=%u\n",
               static_cast<unsigned>(channel), static_cast<unsigned>(type),
               msg.can_id, msg.len, static_cast<unsigned>(msg.fd),
               static_cast<unsigned>(msg.brs));
        return 0;
    }
    printf("[CANFD TX] ch=%u type=0x%02X id=0x%X len=%u fd=%u brs=%u\n",
           channel, type, msg.can_id, msg.len, msg.fd, msg.brs);
    if (!ch->running) {
        return -1;
    }

    if (ch->packet_interval != 0 && ch->last_tx_us != 0) {
        const uint64_t now = now_us();
        const uint64_t elapsed = now - ch->last_tx_us;
        if (elapsed < ch->packet_interval) {
            usleep(static_cast<useconds_t>(ch->packet_interval - elapsed));
        }
    }

    /* The upper-computer data words use the opposite byte order from the
     * CAN FD controller's data words.  Protocol header fields stay unchanged. */
    reverse_data_words(msg.data, msg.len);
    int written = -1;
    if (msg.fd) {
        struct canfd_frame can;
        memset(&can, 0, sizeof(can));
        can.can_id = msg.can_id | (msg.eff ? CAN_EFF_FLAG : 0);
        can.len = msg.len;
        can.flags = (msg.brs ? CANFD_BRS : 0) | (msg.esi ? CANFD_ESI : 0);
        memcpy(can.data, msg.data, msg.len);
        written = static_cast<int>(write(ch->fd, &can, CANFD_MTU));
    } else {
        struct can_frame can;
        memset(&can, 0, sizeof(can));
        can.can_id = msg.can_id |
                     (msg.eff ? CAN_EFF_FLAG : 0) |
                     (msg.rtr ? CAN_RTR_FLAG : 0);
        can.can_dlc = msg.len > 8 ? 8 : msg.len;
        memcpy(can.data, msg.data, can.can_dlc);
        written = static_cast<int>(write(ch->fd, &can, CAN_MTU));
    }
    ch->last_tx_us = now_us();
    const int expected = msg.fd ? static_cast<int>(CANFD_MTU) : static_cast<int>(CAN_MTU);
    if (written != expected) {
        printf("[CANFD] can%u write failed: %s\n",
               static_cast<unsigned>(channel), strerror(errno));
        return -1;
    }
    return 0;
}

extern "C" int canfd_hw_submit_simulator(uint8_t channel, const uint8_t* packet, uint32_t length)
{
    /* TODO(protocol 2.5): store the reply table and answer matching RX frames.
     * The transport/format is already validated by the protocol layer. */
    printf("[CANFD] can%u simulator table entry dropped (not implemented yet, %u bytes)\n",
           static_cast<unsigned>(channel), length);
    (void)packet;
    return 0;
}

extern "C" int canfd_hw_is_stub(void)
{
    return g_any_interface ? 0 : 1;
}

extern "C" void canfd_hw_set_rx_callback(canfd_hw_rx_cb cb, void* user)
{
    pthread_mutex_lock(&g_rx_cb_lock);
    g_rx_cb = cb;
    g_rx_user = user;
    pthread_mutex_unlock(&g_rx_cb_lock);
}

namespace {

void ensure_started()
{
    if (!g_started) {
        canfd_hw_start();
    }
}

}  // namespace
