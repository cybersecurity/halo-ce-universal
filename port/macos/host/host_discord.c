/* Discord RPC uses native Unix sockets, outside the guest Winsock adapter. */
#ifndef HALO_IOS
#include "posix.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int posix_discord_connect(void) {
    const char *directories[6];
    const char *variables[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    char user_temporary[1024];
    int count = 0;
    for (size_t i = 0; i < sizeof(variables) / sizeof(*variables); i++) {
        const char *value = getenv(variables[i]);
        if (value && *value)
            directories[count++] = value;
    }
    /* Finder/Discord launches need not inherit the shell's TMPDIR. */
    size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, user_temporary, sizeof(user_temporary));
    if (size > 0 && size <= sizeof(user_temporary))
        directories[count++] = user_temporary;
    directories[count++] = "/tmp";
    for (int i = 0; i < count; i++) {
        for (int number = 0; number < 10; number++) {
            struct sockaddr_un address = {.sun_family = AF_UNIX};
            int length = snprintf(address.sun_path, sizeof(address.sun_path),
                                  "%s/discord-ipc-%d", directories[i], number);
            if (length < 0 || length >= (int)sizeof(address.sun_path) ||
                access(address.sun_path, F_OK))
                continue;
            address.sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0)
                return -1;
            int enabled = 1, flags = fcntl(fd, F_GETFL);
            if (flags >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
                fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0 &&
                connect(fd, (const struct sockaddr *)&address, address.sun_len) == 0) {
                /* Like upstream's SO_PEERCRED check, only share an invite
                   with a socket belonging to this user. */
                uid_t user;
                gid_t group;
                if (getpeereid(fd, &user, &group) == 0 && user == getuid())
                    return fd;
            }
            close(fd);
        }
    }
    return -1;
}
#endif
