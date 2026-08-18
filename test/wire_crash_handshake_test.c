#include "../host/wire_host.h"

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void read_exact(int fd, char *out, size_t len)
{
    for (size_t i = 0; i < len; i++)
        assert(read(fd, out + i, 1) == 1);
}

static void send_reply(int fd, const char *data)
{
    unsigned sum = 0;
    for (const char *p = data; *p; p++) sum += (uint8_t)*p;
    char packet[600];
    int len = snprintf(packet, sizeof(packet), "$%s#%02x", data, sum & 0xffu);
    assert(len > 0 && (size_t)len < sizeof(packet));
    assert(write(fd, packet, (size_t)len) == len);
    char ack;
    read_exact(fd, &ack, 1);
    assert(ack == '+');
}

static void receive_command(int fd, char expected)
{
    char c;
    read_exact(fd, &c, 1);
    assert(c == '$');
    read_exact(fd, &c, 1);
    assert(c == expected);
    do { read_exact(fd, &c, 1); } while (c != '#');
    char checksum[2];
    read_exact(fd, checksum, sizeof(checksum));
    assert(write(fd, "+", 1) == 1);
}

int main(void)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    signal(SIGALRM, SIG_DFL);
    alarm(5);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(fds[0]);
        send_reply(fds[1], "S0b");
        char registers[137];
        memset(registers, '0', 136);
        registers[136] = '\0';
        receive_command(fds[1], 'g');
        send_reply(fds[1], registers);
        char stack[513];
        memset(stack, '0', 512);
        stack[512] = '\0';
        receive_command(fds[1], 'm');
        send_reply(fds[1], stack);
        receive_command(fds[1], 'm');
        send_reply(fds[1], "E0e");
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    char json[4096];
    assert(wire_dump_crash_to_buf(fds[0], json, sizeof(json)) == 0);
    assert(strstr(json, "\"halt_signal\": 11") != NULL);
    close(fds[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}
