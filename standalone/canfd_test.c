/*
 * canfd_test.c - CAN / CAN FD send and receive test tool
 *
 *   canfd_test rx <ifname>
 *   canfd_test tx <ifname> <id> <byte>... [--fd] [--brs] [--count N]
 *
 * numbers accept 0x prefix or decimal
 * classic CAN: max 8 bytes; FD: use --fd, max 64 bytes
 *
 * build:
 *   arm-linux-gnueabihf-gcc -O2 -static -o canfd_test canfd_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define MAX_DATA 64

static void usage(const char *p)
{
fprintf(stderr,
"usage:\n"
"  %s rx <ifname>\n"
"  %s tx <ifname> <id> <byte>... [--fd] [--brs] [--count N]\n", p, p);
}

static unsigned long parse_num(const char *s)
{
char *end = NULL;
unsigned long v = strtoul(s, &end, 0);
if (end == s || (end && *end)) {
fprintf(stderr, "bad number: '%s'\n", s);
exit(1);
}
return v;
}

static int open_can(const char *ifname)
{
int s, on = 1;
struct ifreq ifr;
struct sockaddr_can addr;

s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
if (s < 0) { perror("socket"); exit(1); }

memset(&ifr, 0, sizeof(ifr));
strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); exit(1); }

memset(&addr, 0, sizeof(addr));
addr.can_family = AF_CAN;
addr.can_ifindex = ifr.ifr_ifindex;
if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
perror("bind"); exit(1);
}

/* must be enabled explicitly, otherwise the kernel drops FD frames */
if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0)
fprintf(stderr, "warn: CAN_RAW_FD_FRAMES: %s\n", strerror(errno));

printf("ifname=%s ifindex=%d, FD frames enabled\n", ifname, ifr.ifr_ifindex);
return s;
}

static void print_frame(const char *dir, const struct canfd_frame *cf, int is_fd)
{
unsigned int len = cf->len;
int i;

if (len > (unsigned int)(is_fd ? MAX_DATA : 8))
len = is_fd ? MAX_DATA : 8;

printf("%s  id=0x%03x  %s  dlc=%u  %s  data:",
       dir, (unsigned int)(cf->can_id & CAN_EFF_MASK),
       (cf->can_id & CAN_EFF_FLAG) ? "ext" : "std",
       len, is_fd ? "FD" : "CAN");
for (i = 0; i < (int)len; i++)
printf(" %02x", cf->data[i]);
if (is_fd && (cf->flags & CANFD_BRS))
printf("  [BRS]");
printf("\n");
fflush(stdout);
}

int main(int argc, char **argv)
{
int s, i, n, count = 1, use_fd = 0, brs = 0;
unsigned long id;
struct canfd_frame frame;

if (argc < 3) { usage(argv[0]); return 1; }

if (!strcmp(argv[1], "rx")) {
s = open_can(argv[2]);
printf("listening on %s, Ctrl+C to stop\n", argv[2]);
for (;;) {
n = read(s, &frame, sizeof(frame));
if (n < 0) {
if (errno == EINTR)
continue;
perror("read");
break;
}
print_frame("RX", &frame, n == CANFD_MTU);
}
close(s);
return 0;
}

if (strcmp(argv[1], "tx")) { usage(argv[0]); return 1; }
if (argc < 4) { usage(argv[0]); return 1; }

s = open_can(argv[2]);
id = parse_num(argv[3]);

memset(&frame, 0, sizeof(frame));
frame.can_id = (canid_t)(id & CAN_EFF_MASK);
if (id > 0x7FF)
frame.can_id |= CAN_EFF_FLAG;

i = 0;
for (n = 4; n < argc; n++) {
if (!strcmp(argv[n], "--fd")) { use_fd = 1; continue; }
if (!strcmp(argv[n], "--brs")) { brs = 1; continue; }
if (!strcmp(argv[n], "--count")) {
if (++n < argc) count = atoi(argv[n]);
continue;
}
if (i >= MAX_DATA) { fprintf(stderr, "too many bytes\n"); return 1; }
frame.data[i++] = (unsigned char)parse_num(argv[n]);
}
frame.len = (unsigned char)i;

if (brs) { use_fd = 1; frame.flags |= CANFD_BRS; }
if (!use_fd && frame.len > 8) {
fprintf(stderr, "classic CAN max 8 bytes, use --fd\n");
return 1;
}

printf("sending %d frame(s): id=0x%lx len=%u %s%s\n",
       count, id, frame.len, use_fd ? "FD" : "CAN", brs ? " BRS" : "");

for (n = 0; n < count; n++) {
ssize_t w = write(s, &frame, use_fd ? CANFD_MTU : CAN_MTU);
if (w < 0)
printf("write #%d failed: %s\n", n + 1, strerror(errno));
else
print_frame("TX", &frame, use_fd);
usleep(100000);
}

close(s);
return 0;
}
