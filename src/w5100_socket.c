/*
 * Copyright (c) 2015 Francesco Balducci
 *
 * This file is part of nucleo_tests.
 *
 *    nucleo_tests is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    nucleo_tests is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public License
 *    along with nucleo_tests.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <file.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#include "w5100.h"
#include "timespec.h"

/******* defines and macros ********/

#define W5100_SOCKET_FREE (-1)

#if !defined(W5100_NO_STATIC_IP) && !defined(W5100_STATIC_IP)
#  define W5100_STATIC_IP
#elif defined(W5100_NO_STATIC_IP) && defined(W5100_STATIC_IP)
#  error "Only one of W5100_NO_STATIC_IP or W5100_STATIC_IP must be defined."
#endif

#ifdef W5100_STATIC_IP
#  ifndef W5100_IP_ADDR
#    define W5100_IP_ADDR "192.168.1.99"
#  endif
#  ifndef W5100_SUBNET
#    define W5100_SUBNET "255.255.255.0"
#  endif
#  ifndef W5100_GATEWAY_ADDR
#    define W5100_GATEWAY_ADDR "192.168.1.1"
#  endif
#endif

enum w5100_socket_state {
    W5100_SOCK_STATE_NONE = 0,
    W5100_SOCK_STATE_CREATED,
    W5100_SOCK_STATE_CONNECTED,
    W5100_SOCK_STATE_BOUND,
    W5100_SOCK_STATE_LISTENING,
    W5100_SOCK_STATE_ACCEPTED,
    W5100_SOCK_STATE_DISCONNECTED,
};

struct timeout_manager {
    int has_timeout;
    struct timespec end;
};

/******* function prototypes ********/

extern
void w5100_socket_init(void);

static
void w5100_command(int isocket, uint8_t cmd);

static
int w5100_sock_write(int fd, char *buf, int len);

static
int w5100_sock_read(int fd, char *buf, int len);

static
int w5100_sock_close(int fd);

static
short w5100_sock_poll(int fd);

static
void timeout_init(const struct timespec *timeout, struct timeout_manager *tom);

static
int timeout_ended(const struct timeout_manager *tom);

/******* global variables ********/

static struct w5100_socket {
    int fd;
    int isocket;
    int domain;
    int type;
    int protocol;
    enum w5100_socket_state state;
    struct sockaddr_in sockname;
    struct sockaddr_in dest_address;
    int can_broadcast;
    struct timespec recv_timeout;
    struct timespec send_timeout;
    struct fd *fd_data;
    struct fd *connection_data;
} w5100_sockets[W5100_N_SOCKETS];

static uint8_t w5100_mac_addr[6] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85};

/******* function definitions ********/

static
struct w5100_socket *get_socket_from_isocket(int isocket)
{
    struct w5100_socket *s;
    
    if (isocket < 0 || isocket >= W5100_N_SOCKETS)
    {
        s = NULL;
    }
    else
    {
        s = &w5100_sockets[isocket];
    }
    return s;
}

static
struct w5100_socket *get_socket_from_fd(int fd)
{
    struct fd *fds;
    struct w5100_socket *s = NULL;
    
    fds = file_struct_get(fd);
    if (fds == NULL)
    {
        errno = EBADF;
    }
    else if (!S_ISSOCK(fds->stat.st_mode))
    {
        errno = ENOTSOCK;
    }
    else if (fds->opaque == NULL)
    {
        errno = EBADF;
    }
    else
    {
        s = fds->opaque;
    }
    return s;
}

static
struct fd *fill_fd_struct(int sockfd, int isocket)
{
    struct fd *fds;
    
    fds = file_struct_get(sockfd);
    
    fds->isatty = 0;
    fds->isopen = 1;
    fds->write = w5100_sock_write;
    fds->read = w5100_sock_read;
    fds->close = w5100_sock_close;
    fds->poll = w5100_sock_poll;
    fds->stat.st_mode = S_IFSOCK|S_IRWXU|S_IRWXG|S_IRWXO;
    fds->status_flags = O_RDWR;
    fds->stat.st_blksize = 1024;
    fds->opaque = &w5100_sockets[isocket];

    return fds;
}

static
uint16_t get_avail_port(void)
{
    uint16_t avail_port;
    int port_used;

    avail_port = htons(0x7FF0);
    do 
    {
        int isocket;

        port_used = 0;

        for (isocket = 0; isocket < W5100_N_SOCKETS; isocket++)
        {
            if (get_socket_from_isocket(isocket)->state != W5100_SOCK_STATE_NONE)
            {
                uint16_t port;

                w5100_read_sock_regx(W5100_Sn_PORT, isocket, &port);
                if (port == avail_port)
                {
                    port_used = 1;
                    avail_port--;
                    break;
                }
            }
        }
    } while(port_used);
    
    return avail_port;
}

static
int socket_alloc(void)
{
    int i;
    int ret;

    for (i = 0; i < W5100_N_SOCKETS; i++)
    {
        if (w5100_sockets[i].fd == W5100_SOCKET_FREE)
        {
            w5100_sockets[i].fd = i;
            break;
        }
    }
    if (i < W5100_N_SOCKETS)
    {
        ret = i;
    }
    else
    {
        errno = ENFILE;
        ret = -1;
    }
    return ret;
}

static
void socket_free(int isocket)
{
    w5100_sockets[isocket].fd = W5100_SOCKET_FREE;
    w5100_sockets[isocket].fd_data = NULL;
    w5100_sockets[isocket].connection_data = NULL;
    w5100_sockets[isocket].state = W5100_SOCK_STATE_NONE;
}

static
int w5100_sock_write(int fd, char *buf, int len)
{
    return send(fd, buf, len, 0);
}

static
int w5100_sock_read(int fd, char *buf, int len)
{
    return recv(fd, buf, len, 0);
}

static
int w5100_sock_close(int fd)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(fd);
    if (s == NULL)
    {
        ret = -1;
    }
    else
    {
        int isocket;
        uint8_t sr;
        
        isocket = s->isocket;

        if ((s->fd_data != NULL) && (s->fd_data->fd == fd))
        {
            s->fd_data->isopen = 0;
            file_free(s->fd_data->fd);
            s->fd_data = NULL;
            if (s->state != W5100_SOCK_STATE_ACCEPTED)
            {
                if (s->state == W5100_SOCK_STATE_CONNECTED)
                {
                    w5100_command(isocket, W5100_CMD_DISCON);
                }
                else
                {
                    w5100_command(isocket, W5100_CMD_CLOSE);
                }
                do {
                    sr = w5100_read_sock_reg(W5100_Sn_SR, isocket);
                } while (sr != W5100_SOCK_CLOSED);
                socket_free(isocket);
            }
            ret = 0;
        }
        else if (s->connection_data == NULL)
        {
            errno = EBADF;
            ret = -1;
        }
        else if (s->connection_data->fd == fd)
        {
            s->connection_data->isopen = 0;
            file_free(s->connection_data->fd);
            s->connection_data = NULL;
            w5100_command(isocket, W5100_CMD_DISCON);
            do {
                sr = w5100_read_sock_reg(W5100_Sn_SR, isocket);
            } while (sr != W5100_SOCK_CLOSED);
            if (s->fd_data == NULL)
            {
                /* underlying listening socket has been already closed. */
                socket_free(isocket);
            }
            else
            {
                /* re-enter listening state */
                w5100_command(isocket, W5100_CMD_OPEN);
                w5100_command(isocket, W5100_CMD_LISTEN);
                do {
                    sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
                } while ((sr != W5100_SOCK_LISTEN) && (sr != W5100_SOCK_ESTABLISHED));
                s->state = W5100_SOCK_STATE_LISTENING;
            }
            ret = 0;
        }
        else
        {
            errno = EBADF;
            ret = -1;
        }
    }
    return ret;
}

static
int socket_create(int type)
{
    int isocket;
    int fd = -1;

    isocket = socket_alloc();
    if (isocket != -1)
    {
        fd = file_alloc();
        if (fd == -1)
        {
            socket_free(isocket);
            isocket = -1;
            errno = ENFILE;
        }
        else
        {
            struct fd *fds;
            struct w5100_socket *s;
            uint8_t sock_mode;
            
            fds = fill_fd_struct(fd, isocket);
            s = get_socket_from_isocket(isocket);

            s->fd = fd;
            s->isocket = isocket;
            s->domain = AF_INET;
            s->type = type;
            s->protocol = 0;
            s->state = W5100_SOCK_STATE_CREATED;
            s->dest_address.sin_family = AF_UNSPEC;
            s->sockname.sin_family = AF_UNSPEC;
            s->fd_data = fds;
            s->connection_data = NULL;
            s->recv_timeout = TIMESPEC_ZERO;
            s->send_timeout = TIMESPEC_ZERO;
            s->can_broadcast = 0;
            
            switch(type)
            {
                case SOCK_STREAM:
                    sock_mode = W5100_SOCK_MODE_TCP;
                    break;
                case SOCK_DGRAM:
                    sock_mode = W5100_SOCK_MODE_UDP;
                    break;
                case SOCK_RAW:
                    sock_mode = W5100_SOCK_MODE_IPRAW;
                    break;
                default:
                    /* should never arrive here. */
                    sock_mode = W5100_SOCK_MODE_TCP;
                    break;
            }
            w5100_write_sock_reg(W5100_Sn_MR, isocket, sock_mode);
        }
    }
    else
    {
        errno = ENOMEM;
    }
    return fd;
}

int socket(int domain, int type, int protocol)
{
    int ret;

    if (domain != AF_INET)
    {
        errno = EAFNOSUPPORT;
        ret = -1;
    }
    else if ((type != SOCK_STREAM) && (type != SOCK_DGRAM) && (type != SOCK_RAW))
    {
        errno = EPROTOTYPE;
        ret = -1;
    }
    else if (protocol != 0)
    {
        errno = EPROTONOSUPPORT;
        ret = -1;
    }
    else
    {
        int fd;
        
        fd = socket_create(type);
        ret = fd;
    }
    return ret;
}

static
void w5100_command(int isocket, uint8_t cmd)
{
    w5100_write_sock_reg(W5100_Sn_CR, isocket, cmd);
    while (w5100_read_sock_reg(W5100_Sn_CR, isocket))
    {
        continue;
    }
}

static
void bind_udp(struct w5100_socket *s, uint16_t port)
{
    uint8_t sr;

    w5100_write_sock_regx(W5100_Sn_PORT, s->isocket, &port);
    w5100_command(s->isocket, W5100_CMD_OPEN);
    do {
        sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
    } while (sr != W5100_SOCK_UDP);
    s->sockname.sin_family = AF_INET;
    s->sockname.sin_addr.s_addr = INADDR_ANY; /* TODO: local IP */
    s->sockname.sin_port = port;
    s->state = W5100_SOCK_STATE_BOUND;
}

static
void check_bind_udp(struct w5100_socket *s)
{
    if (s->state == W5100_SOCK_STATE_CREATED)
    {
        uint16_t port;

        port = get_avail_port();
        bind_udp(s, port);
    }
}

static
int connect_tcp(struct w5100_socket *s, const struct sockaddr *addr, socklen_t addrlen)
{
    int ret;
    
    if ( addr->sa_family != AF_INET )
    {
        errno = EAFNOSUPPORT;
        ret = -1;
    }
    else if (
            (s->state == W5100_SOCK_STATE_CONNECTED)
            ||
            (s->state == W5100_SOCK_STATE_ACCEPTED)
            )
    {
        errno = EISCONN;
        ret = -1;
    }
    else if (s->state == W5100_SOCK_STATE_LISTENING)
    {
        errno = EOPNOTSUPP;
        ret = -1;
    }
    else if (s->state != W5100_SOCK_STATE_CREATED)
    {
        errno = EOPNOTSUPP;
        ret = -1;
    }
    else
    {
        struct sockaddr_in *server;
        uint8_t sr;
        int isocket;

        (void)addrlen;

        isocket = s->isocket;
        server = (struct sockaddr_in *)addr;
        /* TODO: check if already in use EADDRINUSE */
        w5100_write_sock_regx(W5100_Sn_PORT, isocket, &server->sin_port);
        w5100_command(isocket, W5100_CMD_OPEN);
        do {
            sr = w5100_read_sock_reg(W5100_Sn_SR, isocket);
        } while (sr != W5100_SOCK_INIT);

        w5100_write_sock_regx(W5100_Sn_DIPR, isocket, &server->sin_addr.s_addr);
        w5100_write_sock_regx(W5100_Sn_DPORT, isocket, &server->sin_port);
        w5100_command(isocket, W5100_CMD_CONNECT);
        do {
            sr = w5100_read_sock_reg(W5100_Sn_SR, isocket);
        } while ((sr != W5100_SOCK_CLOSED) && (sr != W5100_SOCK_ESTABLISHED));
        if (sr == W5100_SOCK_ESTABLISHED)
        {
            s->state = W5100_SOCK_STATE_CONNECTED;
            s->dest_address = *server;
            ret = 0;
        }
        else if (sr == W5100_SOCK_CLOSED)
        {
            errno = ECONNREFUSED;
            ret = -1;
        }
        else
        {
            errno = ECONNREFUSED; /* TODO: better error? */
            ret = -1;
        }
    }
    return ret;
}

static
int connect_udp(struct w5100_socket *s, const struct sockaddr *addr, socklen_t addrlen)
{
    int ret;
    
    (void)addrlen;

    if (addr->sa_family == AF_UNSPEC)
    {
        s->dest_address.sin_family = AF_UNSPEC; /* reset pre-specified address */
        check_bind_udp(s);
        ret = 0;
    }
    else if (addr->sa_family == AF_INET)
    {
        const struct sockaddr_in *dest_address;
        
        dest_address = (struct sockaddr_in *)addr;
        s->dest_address = *dest_address;
        check_bind_udp(s);
        ret = 0;
    }
    else
    {
        errno = EAFNOSUPPORT;
        ret = -1;
    }

    return ret;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->type == SOCK_STREAM)
    {
        ret = connect_tcp(s, addr, addrlen);
    }
    else if (s->type == SOCK_DGRAM)
    {
        ret = connect_udp(s, addr, addrlen);
    }
    else /* TODO: RAW */
    {
        errno = EAFNOSUPPORT;
        ret = -1;
    }
    return ret;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if ( addr->sa_family != AF_INET )
    {
        errno = EAFNOSUPPORT;
        ret = -1;
    }
    else if (s->state != W5100_SOCK_STATE_CREATED)
    {
        errno = EINVAL;
        ret = 1;
    }
    else if (s->type == SOCK_STREAM)
    {
        struct sockaddr_in *server;
        uint8_t sr;
        uint8_t sr_end;

        (void)addrlen;
        
        server = (struct sockaddr_in *)addr;
        /* TODO: check if already in use EADDRINUSE */
        w5100_write_sock_regx(W5100_Sn_PORT, s->isocket, &server->sin_port);
        w5100_command(s->isocket, W5100_CMD_OPEN);
        sr_end = W5100_SOCK_INIT;
        do {
            sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
        } while (sr != sr_end);
        s->sockname = *server;
        s->state = W5100_SOCK_STATE_BOUND;
        ret = 0;
    }
    else if (s->type == SOCK_DGRAM)
    {
        struct sockaddr_in *server;
        (void)addrlen;

        server = (struct sockaddr_in *)addr;
        bind_udp(s, server->sin_port);
        ret = 0;
    }
    else
    {
        /* TODO: RAW */
        errno = EBADF;
        ret = -1;
    }
    return ret;
}

int listen(int sockfd, int backlog)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->state != W5100_SOCK_STATE_BOUND)
    {
        errno = EDESTADDRREQ;
        ret = 1;
    }
    else if (s->type != SOCK_STREAM) /* UPD and RAW */
    {
        errno = EOPNOTSUPP;
        ret = -1;
    }
    else /* TCP */
    {
        uint8_t sr;
        /* TODO: check if already in use EADDRINUSE */
        (void)backlog; /* ignoring the hint because we can't do anything about it. */
        w5100_command(s->isocket, W5100_CMD_LISTEN);
        do {
            sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
        } while ((sr != W5100_SOCK_LISTEN) && (sr != W5100_SOCK_ESTABLISHED));
        s->state = W5100_SOCK_STATE_LISTENING;
        ret = 0;
    }
    return ret;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->state != W5100_SOCK_STATE_LISTENING)
    {
        errno = EINVAL;
        ret = 1;
    }
    else if (s->type != SOCK_STREAM) /* UDP or RAW */
    {
        errno = EOPNOTSUPP;
        ret = -1;
    }
    else /* TCP */
    {
        uint8_t sr;
        int newsockfd;
        int nonblock;

        nonblock = s->fd_data->status_flags & O_NONBLOCK;

        do
        {
            sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
            if (sr == W5100_SOCK_ESTABLISHED)
            {
                newsockfd = file_alloc();
                if (newsockfd == -1)
                {
                    errno = ENFILE;
                    /* go again into listen state */
                    w5100_command(s->isocket, W5100_CMD_CLOSE);
                    w5100_command(s->isocket, W5100_CMD_OPEN);
                    w5100_command(s->isocket, W5100_CMD_LISTEN);
                }
                else
                {
                    struct sockaddr_in *client;

                    s->state = W5100_SOCK_STATE_ACCEPTED;
                    s->connection_data = fill_fd_struct(newsockfd, s->isocket);
                    
                    if (addr != NULL)
                    {
                        if (addrlen != NULL)
                        {
                            *addrlen = sizeof(struct sockaddr_in);
                        }
                        client = (struct sockaddr_in *)addr;
                        addr->sa_family = AF_INET;
                        
                        w5100_read_sock_regx(W5100_Sn_DIPR, s->isocket, &client->sin_addr.s_addr);
                        w5100_read_sock_regx(W5100_Sn_DPORT, s->isocket, &client->sin_port);
                    }
                }
                ret = newsockfd;
                break;
            }
            else if (nonblock)
            {
                errno = EAGAIN;
                ret = -1;
                break;
            }
        } while(1);
    }
    return ret;
}

static
void timeout_init(const struct timespec *timeout, struct timeout_manager *tom)
{
    tom->has_timeout = (timespec_diff(&TIMESPEC_ZERO, timeout, NULL) != 0);

    if (tom->has_timeout)
    {
        clock_gettime(CLOCK_MONOTONIC, &tom->end);
        timespec_incr(&tom->end, timeout);
    }
}

static
int timeout_ended(const struct timeout_manager *tom)
{
    int ret;
    struct timespec cur;

    if (tom->has_timeout)
    {
        clock_gettime(CLOCK_MONOTONIC, &cur);
        ret = (timespec_diff(&tom->end, &cur, NULL) < 0);
        if (ret)
        {
            errno = EAGAIN;
        }
    }
    else
    {
        ret = 0;
    }

    return ret;
}

static
uint16_t get_tx_size(int isocket)
{
    (void)isocket;
    return 0x800; /* 2KiB */
}

static
uint16_t get_tx_mask(int isocket)
{
    return get_tx_size(isocket) - 1; /* size is always power of 2 */
}

static
uint16_t get_tx_base(int isocket)
{
    return W5100_TX_MEM_BASE + get_tx_size(isocket) * isocket;
}

static
uint16_t get_rx_size(int isocket)
{
    (void)isocket;
    return 0x800; /* 2KiB */
}

static
uint16_t get_rx_mask(int isocket)
{
    return get_rx_size(isocket) - 1; /* size is always power of 2 */
}

static
uint16_t get_rx_base(int isocket)
{
    return W5100_RX_MEM_BASE + get_rx_size(isocket) * isocket;
}

static
int manage_disconnect(struct w5100_socket *s)
{
    int ret;
    uint8_t sr;
    
    sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
    if (sr != W5100_SOCK_ESTABLISHED)
    {
        if (sr == W5100_SOCK_CLOSE_WAIT)
        {
            errno = ECONNRESET;
        }
        else
        {
            errno = ETIMEDOUT;
        }
        w5100_command(s->isocket, W5100_CMD_DISCON);
        do
        {
            sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
        } while(sr != W5100_SOCK_CLOSED);
        if (
                (s->state == W5100_SOCK_STATE_ACCEPTED)
                &&
                (s->fd_data != NULL)
           )
        {
            /* re-enter listening state */
            w5100_command(s->isocket, W5100_CMD_OPEN);
            w5100_command(s->isocket, W5100_CMD_LISTEN);
            do {
                sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
            } while ((sr != W5100_SOCK_LISTEN) && (sr != W5100_SOCK_ESTABLISHED));
            s->state = W5100_SOCK_STATE_LISTENING;
        }
        else
        {
            s->state = W5100_SOCK_STATE_DISCONNECTED;
        }
        ret = -1;
    }
    else
    {
        ret = 0;
    }
    return ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    return recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

static
uint16_t read_buf_len(int isocket)
{
    uint16_t toread;

    w5100_read_sock_regx(W5100_Sn_RX_RSR, isocket, &toread);
    toread = ntohs(toread);

    return toread;
}

static
void read_buf_sure(int isocket, void *buf, size_t len, uint16_t *pread)
{
    uint16_t offset;
    uint16_t phys;
    uint16_t toread1;
    uint16_t toread2;
    uint8_t *bytes = buf;

    offset = *pread & get_rx_mask(isocket);
    phys = get_rx_base(isocket) + offset;
    if (offset + len > get_rx_size(isocket))
    {
        toread1 = get_rx_size(isocket) - offset;
        toread2 = len - toread1;
    }
    else
    {
        toread1 = len;
        toread2 = 0;
    }
    if (toread1 > 0)
    {
        w5100_read_mem(phys, &bytes[0], toread1);
    }
    if (toread2 > 0)
    {
        w5100_read_mem(get_rx_base(isocket), &bytes[toread1], toread2);
    }
    *pread = *pread + len;
}

static
uint16_t read_buf_pstart(int isocket)
{
    uint16_t pread;

    w5100_read_sock_regx(W5100_Sn_RX_RD, isocket, &pread);
    pread = ntohs(pread);

    return pread;
}

static
void read_buf_recv(int isocket, uint16_t pstop)
{
    pstop = htons(pstop);
    w5100_write_sock_regx(W5100_Sn_RX_RD, isocket, &pstop);
    w5100_command(isocket, W5100_CMD_RECV);
}

static
uint16_t read_buf(int isocket, void *buf, size_t len)
{
    uint16_t toread;

    toread = read_buf_len(isocket);
    if (toread != 0)
    {
        uint16_t pread;
        
        if (len > toread)
        {
            len = toread;
        }
        pread = read_buf_pstart(isocket);
        read_buf_sure(isocket, buf, len, &pread);
        read_buf_recv(isocket, pread);
    }
    else
    {
        len = 0;
    }
    return len;
}

static
uint16_t write_buf_len(int isocket)
{
    uint16_t nfree;

    w5100_read_sock_regx(W5100_Sn_TX_FSR, isocket, &nfree);
    nfree = ntohs(nfree);

    return nfree;
}

static
uint16_t write_buf_pstart(int isocket)
{
    uint16_t pwrite;

    w5100_read_sock_regx(W5100_Sn_TX_WR, isocket, &pwrite);
    pwrite = ntohs(pwrite);

    return pwrite;
}

static
void write_buf_send(int isocket, uint16_t pstop)
{
    pstop = htons(pstop);
    w5100_write_sock_regx(W5100_Sn_TX_WR, isocket, &pstop);
    w5100_command(isocket, W5100_CMD_SEND);
}

static
void write_buf_sure(int isocket, const void *buf, size_t len, uint16_t *pwrite)
{
    uint16_t offset;
    uint16_t phys;
    uint16_t towrite1;
    uint16_t towrite2;
    const uint8_t *bytes = buf;
    
    offset = *pwrite & get_tx_mask(isocket);
    phys = offset + get_tx_base(isocket);
    if (len + offset > get_tx_size(isocket))
    {
        towrite1 = get_tx_size(isocket) - offset;
        towrite2 = len - towrite1;
    }
    else
    {
        towrite1 = len;
        towrite2 = 0;
    }
    if (towrite1 > 0)
    {
        w5100_write_mem(phys, &bytes[0], towrite1);
    }
    if (towrite2 > 0)
    {
        w5100_write_mem(get_tx_base(isocket), &bytes[towrite1], towrite2);
    }
    *pwrite += len;
}

static
uint16_t write_buf(int isocket, const void *buf, size_t len)
{
    uint16_t nfree;

    nfree = write_buf_len(isocket);
    if (nfree > 0)
    {
        uint16_t pwrite;

        if (len > nfree)
        {
            len = nfree;
        }
        pwrite = write_buf_pstart(isocket);
        write_buf_sure(isocket, buf, len, &pwrite);
        write_buf_send(isocket, pwrite);
    }
    else
    {
        len = 0;
    }
    return len;
}

ssize_t recvfrom(int sockfd, void *__restrict buf, size_t len, int flags,
        struct sockaddr *__restrict address, socklen_t *__restrict address_len)
{
    ssize_t ret;
    struct w5100_socket *s;

    (void)flags; /* TODO */

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (
            (s->type == SOCK_STREAM)
            &&
            (s->state != W5100_SOCK_STATE_ACCEPTED)
            &&
            (s->state != W5100_SOCK_STATE_CONNECTED)
            )
    {
        errno = ENOTCONN;
        ret = 1;
    }
    else if (
            (s->type == SOCK_DGRAM)
            &&
            (s->state != W5100_SOCK_STATE_BOUND)
            &&
            (s->state != W5100_SOCK_STATE_CREATED)
            )
    {
        errno = ENOTCONN;
        ret = 1;
    }
    else if (len == 0)
    {
        ret = 0;
    }
    else
    {
        struct timeout_manager tom;
        int nonblock;

        nonblock = s->fd_data->status_flags & O_NONBLOCK;

        if (!nonblock)
        {
            timeout_init(&s->recv_timeout, &tom);
        }
        do
        {
            if (s->type == SOCK_STREAM)
            {
                uint16_t nread;

                nread = read_buf(s->isocket, buf, len);
                if (nread != 0)
                {
                    (void)address; /* TODO: fill */
                    (void)address_len; /* TODO: fill */
                    ret = nread;
                    break;
                }
                else if (manage_disconnect(s) == -1)
                {
                    /* TODO: return 0 on orderly shutdown */
                    ret = -1;
                    break;
                }
            }
            else if (s->type == SOCK_DGRAM)
            {
                uint16_t toread;
                uint8_t header[8];

                toread = read_buf_len(s->isocket);

                if (toread >= sizeof(header))
                {
                    uint16_t pread;
                    uint16_t msg_len;

                    pread = read_buf_pstart(s->isocket);
                    read_buf_sure(s->isocket, header, sizeof(header), &pread);
                    if ((address != NULL) && (address_len != NULL))
                    {
                        struct sockaddr_in *peer;
                        /* TODO: check address_len in input and truncate in case */
                        
                        peer = (struct sockaddr_in *)address;
                        memcpy(&peer->sin_addr.s_addr, &header[0], 4);
                        memcpy(&peer->sin_port, &header[4], 2);
                        *address_len = sizeof(struct sockaddr_in);
                    }
                    memcpy(&msg_len, &header[6], 2);
                    msg_len = ntohs(msg_len);
                    read_buf_sure(s->isocket, buf, msg_len, &pread);
                    read_buf_recv(s->isocket, pread);
                    ret = msg_len;
                    break;
                }
            }
            if (nonblock)
            {
                ret = -1;
                errno = EAGAIN;
                break;
            }
            else if (timeout_ended(&tom))
            {
                ret = -1;
                break;
            }
        } while(1);
    }
    return ret;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    ssize_t ret;
    struct w5100_socket *s;

    (void)flags; /* TODO */

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->type == SOCK_DGRAM)
    {
        if (s->dest_address.sin_family == AF_UNSPEC)
        {
            errno = EDESTADDRREQ;
            ret = -1;
        }
        else
        {
            ret = sendto(sockfd, buf, len, flags, (const struct sockaddr *)&s->dest_address, sizeof(s->dest_address));
        }
    }
    else if (s->type != SOCK_STREAM) /* RAW */
    {
        errno = EDESTADDRREQ;
        ret = -1;
    }
    else if (
            (s->state != W5100_SOCK_STATE_ACCEPTED)
            &&
            (s->state != W5100_SOCK_STATE_CONNECTED)
            )
    {
        errno = ENOTCONN;
        ret = 1;
    }
    else
    {
        size_t towrite;
        struct timeout_manager tom;
        const uint8_t *bytes;
        int nonblock;

        nonblock = s->fd_data->status_flags & O_NONBLOCK;
        towrite = len;
        bytes = buf;
        if (!nonblock)
        {
            timeout_init(&s->send_timeout, &tom);
        }

        while (towrite > 0)
        {
            size_t written;

            written = write_buf(s->isocket, bytes, towrite);
            if (written > 0)
            {
                bytes += written;
                towrite -= written;
                if (nonblock)
                {
                    break;
                }
            }
            else if (manage_disconnect(s) == -1)
            {
                ret = -1;
                break;
            }
            else if (nonblock)
            {
                errno = EAGAIN;
                break;
            }
            else if (timeout_ended(&tom))
            {
                break;
            }
        }
        ret = len - towrite;
    }
    return ret;
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_address, socklen_t dest_len)
{
    ssize_t ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->type == SOCK_STREAM)
    {
        (void)dest_address; /* ignore */
        (void)dest_len; /* ignore */
        ret = send(sockfd, buf, len, flags);
    }
    else if (s->type == SOCK_DGRAM)
    {
        const struct sockaddr_in *peer;

        check_bind_udp(s);
        peer = (struct sockaddr_in *)dest_address;

        if (len > get_tx_size(s->isocket))
        {
            errno = EMSGSIZE;
            ret = -1;
        }
        else if (dest_address == NULL)
        {
            errno = EDESTADDRREQ;
            ret = -1;
        }
        else if ((peer->sin_addr.s_addr == INADDR_BROADCAST) && !s->can_broadcast)
        {
            errno = EINVAL;
            ret = -1;
        }
        else
        {
            struct timeout_manager tom;
            int nonblock;

            nonblock = s->fd_data->status_flags & O_NONBLOCK;
            if (!nonblock)
            {
                timeout_init(&s->send_timeout, &tom);
            }
            do
            {
                if (write_buf_len(s->isocket) >= len)
                {
                    w5100_write_sock_regx(W5100_Sn_DIPR, s->isocket, &peer->sin_addr.s_addr);
                    w5100_write_sock_regx(W5100_Sn_DPORT, s->isocket, &peer->sin_port);

                    ret = write_buf(s->isocket, buf, len);
                    break;
                }
                else if (nonblock)
                {
                    errno = EAGAIN;
                    ret = 1;
                    break;
                }
                else if (timeout_ended(&tom))
                {
                    ret = -1;
                    break;
                }
            } while(1); /* TODO: non blocking */
        }
    }
    else /* TODO: RAW */
    {
        errno = EBADF;
        ret = -1;
    }
    return ret;
}

static
short w5100_sock_poll_rw(int isocket)
{
    short ret;
    uint16_t toread;
    uint16_t towrite;

    toread = read_buf_len(isocket);
    towrite = write_buf_len(isocket);

    ret = 0;
    if (toread > 0)
    {
        ret |= POLLRDNORM|POLLIN;
    }
    if (towrite > 0)
    {
        ret |= POLLWRNORM|POLLOUT;
    }

    return ret;
}

static
short w5100_sock_poll(int fd)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(fd);
    if (s == NULL)
    {
        ret = POLLNVAL;
    }
    else if(s->state == W5100_SOCK_STATE_LISTENING)
    {
        uint8_t sr;

        sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);
        if (sr == W5100_SOCK_ESTABLISHED)
        {
            ret = POLLRDNORM|POLLIN;
        }
        else
        {
            ret = 0;
        }
    }
    else if (
                (s->type == SOCK_STREAM) &&
                (
                    (s->state == W5100_SOCK_STATE_CONNECTED) ||
                    (s->state == W5100_SOCK_STATE_ACCEPTED)
                )
            )
    {
        uint8_t sr;

        sr = w5100_read_sock_reg(W5100_Sn_SR, s->isocket);

        if (sr != W5100_SOCK_ESTABLISHED)
        {
            ret = POLLHUP;
        }
        else
        {
            ret = w5100_sock_poll_rw(s->isocket);
        }
    }
    else if (
                (s->type == SOCK_DGRAM) &&
                (
                    (s->state == W5100_SOCK_STATE_CONNECTED) ||
                    (s->state == W5100_SOCK_STATE_BOUND)
                )
            )
    {
        ret = w5100_sock_poll_rw(s->isocket);
    }
    else
    {
        ret = POLLNVAL;
    }

    return ret;
}

int setsockopt(int sockfd, int level, int option_name, const void *option_value, socklen_t option_len)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (level != SOL_SOCKET)
    {
        ret = -1;
        errno = EINVAL;
    }
    else
    {
        /* TODO: check and fill option_len */
        (void)option_len;
        switch (option_name)
        {
            case SO_BROADCAST:
                s->can_broadcast = ((*(int *)option_value) != 0);
                ret = 0;
                break;
            case SO_RCVTIMEO:
                timeval_to_timespec((const struct timeval *)option_value, &s->recv_timeout);
                ret = 0;
                break;
            case SO_SNDTIMEO:
                timeval_to_timespec((const struct timeval *)option_value, &s->send_timeout);
                ret = 0;
                break;
            default:
                ret = -1;
                errno = EINVAL;
                break;
        }
    }

    return ret;
}

int getsockopt(int sockfd, int level, int option_name, void *__restrict option_value, socklen_t *__restrict option_len)
{
    int ret;
    struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (level != SOL_SOCKET)
    {
        ret = -1;
        errno = EINVAL;
    }
    else
    {
        /* TODO: check and fill option_len */
        (void)option_len;
        switch (option_name)
        {
            case SO_ACCEPTCONN:
                *(int *)option_value = (s->state == W5100_SOCK_STATE_LISTENING);
                ret = 0;
                break;
            case SO_BROADCAST:
                ret = 0;
                break;
            case SO_RCVTIMEO:
                timespec_to_timeval(&s->recv_timeout, (struct timeval *)option_value);
                ret = 0;
                break;
            case SO_SNDTIMEO:
                timespec_to_timeval(&s->send_timeout, (struct timeval *)option_value);
                ret = 0;
                break;
            case SO_TYPE:
                *(int *)option_value = s->type;
                ret = 0;
                break;
            default:
                ret = -1;
                errno = EINVAL;
                break;
        }
    }

    return ret;
}

static
int get_sockaddr_in(
        const struct sockaddr_in *sock_addr,
        struct sockaddr *address,
        socklen_t *address_len)
{
    int ret;
    socklen_t len;

    if (address_len != NULL)
    {
        len = *address_len;
        if ((size_t)len > sizeof(struct sockaddr_in))
        {
            len = sizeof(struct sockaddr_in);
        }
        *address_len = len;
    }
    else
    {
        len = sizeof(struct sockaddr_in);
    }
    if (address == NULL)
    {
        errno = EINVAL;
        ret = -1;
    }
    else
    {
        memcpy(address, sock_addr, len);
        ret = 0;
    }
    return ret;
}

int getsockname(
        int sockfd,
        struct sockaddr *address,
        socklen_t *address_len)
{
    int ret;
    const struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->sockname.sin_family == AF_UNSPEC)
    {
        errno = EINVAL;
        ret = -1;
    }
    else
    {
        ret = get_sockaddr_in(&s->sockname, address, address_len);
    }

    return ret;
}

int getpeername(
        int sockfd,
        struct sockaddr *address,
        socklen_t *address_len)
{
    int ret;
    const struct w5100_socket *s;

    s = get_socket_from_fd(sockfd);
    if (s == NULL)
    {
        ret = -1;
    }
    else if (s->dest_address.sin_family == AF_UNSPEC)
    {
        errno = ENOTCONN;
        ret = -1;
    }
    else
    {
        ret = get_sockaddr_in(&s->dest_address, address, address_len);
    }

    return ret;
}

__attribute__((__constructor__))
void w5100_socket_init(void)
{
    int i;

    w5100_init();

    w5100_write_reg(W5100_MR, W5100_MODE_RST); /* RST */
    while(w5100_read_reg(W5100_MR) & W5100_MODE_RST) /* RST bit clears by itself */
    {
        continue;
    }
    
    for (i = 0; i < W5100_N_SOCKETS; i++)
    {
        socket_free(i);
    }
    w5100_write_reg(W5100_RMSR, 0x55); /* 2KiB per socket */
    w5100_write_reg(W5100_TMSR, 0x55); /* 2KiB per socket */
    w5100_write_regx(W5100_SHAR, w5100_mac_addr);

#ifdef W5100_STATIC_IP
    do {
        in_addr_t addr;

        addr = inet_addr(W5100_IP_ADDR);
        w5100_write_regx(W5100_SIPR, &addr);
        addr = inet_addr(W5100_GATEWAY_ADDR);
        w5100_write_regx(W5100_GAR, &addr);
        addr = inet_addr(W5100_SUBNET);
        w5100_write_regx(W5100_SUBR, &addr);
    } while(0);
#endif
}

