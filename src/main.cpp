#include "canfd_service.h"

#include <signal.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);

    if (canfd_service_start() != 0) {
        return 1;
    }

    while (1) {
        sleep(1);
    }
    return 0;
}
