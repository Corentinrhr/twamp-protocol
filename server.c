/*
 * Name: Emma Mirică
 * Project: TWAMP Protocol
 * Class: OSS
 * Email: emma.mirica@cti.pub.ro
 * Contributions: stephanDB
 *
 * Source: server.c
 * Note: contains the TWAMP server implementation
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include "twamp.h"

#ifndef IPV6_HOPLIMIT
#define IPV6_HOPLIMIT IPV6_UNICAST_HOPS
#endif

#define MAX_CLIENTS 64
#define MAX_SESSIONS_PER_CLIENT 10
#define PORTBASE_MIN 20000
#define PORTBASE_MAX 30000
/* Seconds before an idle kConnected/kConfigured client is dropped */
#define CLIENT_IDLE_TIMEOUT 30

typedef enum {
    kOffline = 0,
    kConnected,
    kConfigured,
    kTesting
} ClientStatus;

struct active_session {
    int socket;
    RequestSession req;
    uint16_t server_oct;
    uint32_t sid_addr;
    TWAMPTimestamp sid_time;
    uint32_t sid_rand;
    uint32_t seq_nb;
    uint32_t snd_nb;
    uint32_t fw_msg;
    uint32_t fw_lst_msg;
};

struct client_info {
    ClientStatus status;
    int socket;
    struct sockaddr_in addr;
    struct sockaddr_in6 addr6;
    int mode;
    int sess_no;
    struct timeval connect_time;   /* when TCP was accepted */
    struct timeval shutdown_time;
    struct active_session sessions[MAX_SESSIONS_PER_CLIENT];
};

const uint8_t One = 1;
static int fd_max = 0;
static int port_min = PORTBASE_MIN;
static int port_max = PORTBASE_MAX;
static int tcp_port = SERVER_PORT;
static enum Mode authmode = kModeUnauthenticated;
static int used_sockets = 0;
static fd_set read_fds;
static uint16_t servo = 0;
static int socket_family = AF_INET;
TWAMPTimestamp ZeroT = { 0, 0 };

/* ------------------------------------------------------------------ */
/* Recompute fd_max by scanning all active fds                         */
/* ------------------------------------------------------------------ */
static void recompute_fd_max(int listenfd, struct client_info *clients, int max_clients) {
    int new_max = listenfd;
    int i, j;
    for (i = 0; i < max_clients; i++) {
        if (clients[i].status == kOffline) continue;
        if (clients[i].socket > new_max) new_max = clients[i].socket;
        for (j = 0; j < clients[i].sess_no; j++)
            if (clients[i].sessions[j].socket > new_max)
                new_max = clients[i].sessions[j].socket;
    }
    fd_max = new_max;
}

static void usage(char *progname) {
    fprintf(stderr, "Usage: %s [options]\n", progname);
    fprintf(stderr, "\nWhere \"options\" are:\n");
    fprintf(stderr,
        " -a authmode    Default is Unauthenticated\n"
        " -p port_min    Port min for Test receivers (>1063)\n"
        " -q port_max    Port max for Test receivers\n"
        " -c port        Listening TCP port (default: 862)\n"
        " -o servo       2 Octets to be reflected (<65536)\n"
        " -6             Use IPv6\n"
        " -h             Prints this help message and exits\n");
}

static int parse_options(char *progname, int argc, char *argv[]) {
    (void)progname;
    int opt;
    while ((opt = getopt(argc, argv, "a:p:q:c:o:h6")) != -1) {
        switch (opt) {
        case 'a':
            authmode = strtol(optarg, NULL, 10);
            if (authmode < 0 || authmode > 511) return 1;
            break;
        case 'p':
            port_min = atoi(optarg);
            if (port_min < 1024 || port_min > 64535) port_min = PORTBASE_MIN;
            break;
        case 'q':
            port_max = atoi(optarg);
            break;
        case 'c':
            tcp_port = atoi(optarg);
            if (tcp_port < 1 || tcp_port > 65535) tcp_port = SERVER_PORT;
            break;
        case 'o':
            servo = strtol(optarg, NULL, 10);
            authmode = authmode | kModeReflectOctets;
            break;
        case '6':
            socket_family = AF_INET6;
            break;
        case 'h':
        default:
            return 1;
        }
    }
    if (port_max <= port_min || port_max > 65535) {
        fprintf(stderr, "Invalid port range [%d-%d], using defaults [%d-%d]\n",
            port_min, port_max, PORTBASE_MIN, PORTBASE_MAX);
        port_min = PORTBASE_MIN;
        port_max = PORTBASE_MAX;
    }
    if (tcp_port >= port_min && tcp_port <= port_max) {
        fprintf(stderr, "TCP port %d conflicts with UDP range [%d-%d], "
            "reverting to default %d\n",
            tcp_port, port_min, port_max, SERVER_PORT);
        tcp_port = SERVER_PORT;
    }
    fprintf(stderr, "Config: TCP control port=%d, UDP test range=[%d-%d]\n",
        tcp_port, port_min, port_max);
    return 0;
}

static void cleanup_client(struct client_info *client) {
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));
    fprintf(stderr, "Cleanup client %s (status=%d)\n", str_client, client->status);

    FD_CLR(client->socket, &read_fds);
    close(client->socket);
    used_sockets--;

    int i;
    for (i = 0; i < client->sess_no; i++) {
        if (client->sessions[i].socket > 0) {
            FD_CLR(client->sessions[i].socket, &read_fds);
            close(client->sessions[i].socket);
            client->sessions[i].socket = -1;
            used_sockets--;
        }
    }
    memset(client, 0, sizeof(struct client_info));
    client->status = kOffline;
}

static int find_empty_client(struct client_info *clients, int max_clients) {
    int i;
    for (i = 0; i < max_clients; i++)
        if (clients[i].status == kOffline) return i;
    return -1;
}

static int send_greeting(uint16_t mode_mask, struct client_info *client) {
    int socket = client->socket;
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));

    ServerGreeting greet;
    memset(&greet, 0, sizeof(greet));
    greet.Modes = htonl(client->mode & mode_mask);
    int i;
    for (i = 0; i < 16; i++) greet.Challenge[i] = rand() % 16;
    for (i = 0; i < 16; i++) greet.Salt[i] = rand() % 16;
    greet.Count = htonl(1 << 10);

    int rv = send(socket, &greet, sizeof(greet), 0);
    if (rv < 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to send ServerGreeting");
        cleanup_client(client);
    } else if ((authmode & 0x000F) == 0) {
        fprintf(stderr, "[%s] Mode 0, aborting\n", str_client);
        cleanup_client(client);
    } else {
        printf("Sent ServerGreeting to %s\n", str_client);
    }
    return rv;
}

static int receive_greet_response(struct client_info *client) {
    int socket = client->socket;
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));

    SetUpResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rv = recv(socket, &resp, sizeof(resp), 0);
    if (rv <= 32) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to receive SetUpResponse");
        cleanup_client(client);
    } else {
        fprintf(stderr, "Received SetUpResponse from %s mode=%d\n",
            str_client, ntohl(resp.Mode));
        if ((ntohl(resp.Mode) & client->mode & 0x000F) == 0) {
            fprintf(stderr, "[%s] No usable Mode\n", str_client);
            rv = 0;
        }
        client->mode = ntohl(resp.Mode);
    }
    return rv;
}

static int send_start_serv(struct client_info *client, TWAMPTimestamp StartTime) {
    int socket = client->socket;
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));

    ServerStart msg;
    memset(&msg, 0, sizeof(msg));
    msg.Accept = ((StartTime.integer == 0) && (StartTime.fractional == 0))
        ? kAspectNotSupported : kOK;
    msg.StartTime = StartTime;

    int rv = send(socket, &msg, sizeof(msg), 0);
    if (rv <= 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to send ServerStart");
        cleanup_client(client);
    } else {
        client->status = kConfigured;
        printf("ServerStart sent to %s\n", str_client);
        if (msg.Accept == kAspectNotSupported)
            cleanup_client(client);
    }
    return rv;
}

static int send_start_ack(struct client_info *client) {
    StartACK ack;
    memset(&ack, 0, sizeof(ack));
    ack.Accept = kOK;
    int rv = send(client->socket, &ack, sizeof(ack), 0);
    if (rv <= 0) perror("Failed to send StartACK");
    else printf("StartACK sent\n");
    return rv;
}

static int receive_start_sessions(struct client_info *client) {
    int rv = send_start_ack(client);
    if (rv <= 0) return rv;
    int i;
    for (i = 0; i < client->sess_no; i++) {
        FD_SET(client->sessions[i].socket, &read_fds);
        if (fd_max < client->sessions[i].socket)
            fd_max = client->sessions[i].socket;
    }
    client->status = kTesting;
    fprintf(stderr, "\tSnd@\t,\tTime\t, Snd#\t, Rcv#\t, SndPt\t,"
        " RcvPt\t, Sync\t, TTL\t, SndTOS, FW_TOS, Int D\t, FWD [ms]\n");
    return rv;
}

static int receive_stop_sessions(struct client_info *client) {
    gettimeofday(&client->shutdown_time, NULL);
    return 0;
}

static int send_accept_session(struct client_info *client, RequestSession *req) {
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));

    AcceptSession acc;
    memset(&acc, 0, sizeof(acc));

    if ((used_sockets < 200) && (client->sess_no < MAX_SESSIONS_PER_CLIENT)) {
        int testfd = socket(socket_family, SOCK_DGRAM, 0);
        if (testfd < 0) {
            perror("Error opening UDP test socket");
            return -1;
        }
        int port_range = port_max - port_min + 1;
        int check_time = CHECK_TIMES;

        if (socket_family == AF_INET6) {
            struct sockaddr_in6 local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin6_family = AF_INET6;
            local_addr.sin6_addr = in6addr_any;
            local_addr.sin6_port = htons(port_min + rand() % port_range);
            while (check_time-- && bind(testfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
                local_addr.sin6_port = htons(port_min + rand() % port_range);
            if (check_time >= 0) req->ReceiverPort = local_addr.sin6_port;
        } else {
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            local_addr.sin_port = htons(port_min + rand() % port_range);
            while (check_time-- && bind(testfd, (struct sockaddr *)&local_addr, sizeof(struct sockaddr)) < 0)
                local_addr.sin_port = htons(port_min + rand() % port_range);
            if (check_time >= 0) req->ReceiverPort = local_addr.sin_port;
        }

        if (check_time >= 0) {
            acc.Accept = kOK;
            acc.Port = req->ReceiverPort;
            client->sessions[client->sess_no].socket = testfd;
            client->sessions[client->sess_no].req = *req;
            memcpy(acc.SID, &req->ReceiverAddress, 4);
            TWAMPTimestamp sidtime = get_timestamp();
            memcpy(&acc.SID[4], &sidtime, 8);
            int k;
            for (k = 0; k < 4; k++) acc.SID[12 + k] = rand() % 256;
            memcpy(&client->sessions[client->sess_no].sid_addr, &acc.SID, 4);
            memcpy(&client->sessions[client->sess_no].sid_rand, &acc.SID[12], 4);
            client->sessions[client->sess_no].sid_time = sidtime;
            if ((client->mode & kModeReflectOctets) == kModeReflectOctets) {
                acc.ReflectedOctets = req->OctetsToBeReflected;
                client->sessions[client->sess_no].server_oct = servo;
                acc.ServerOctets = htons(servo);
            }
            set_socket_option(testfd, HDR_TTL);
            set_socket_tos(testfd, client->sessions[client->sess_no].req.TypePDescriptor << 2);
            client->sess_no++;
            used_sockets++;
        } else {
            close(testfd);
            fprintf(stderr, "[%s] kTemporaryResourceLimitation: bind failed\n", str_client);
            acc.Accept = kTemporaryResourceLimitation;
            acc.Port = 0;
        }
    } else {
        fprintf(stderr, "[%s] kTemporaryResourceLimitation: used_sockets=%d sess_no=%d\n",
            str_client, used_sockets, client->sess_no);
        acc.Accept = kTemporaryResourceLimitation;
        acc.Port = 0;
    }
    return send(client->socket, &acc, sizeof(acc), 0);
}

static int receive_request_session(struct client_info *client, RequestSession *req) {
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));
    fprintf(stderr, "Server received RequestTWSession from %s\n", str_client);
    int rv = send_accept_session(client, req);
    if (rv <= 0) {
        fprintf(stderr, "[%s] Failed to send AcceptSession\n", str_client);
    }
    return rv;
}

static int receive_test_message(struct client_info *client, int session_index) {
    struct sockaddr_in addr;
    struct sockaddr_in6 addr6;
    socklen_t len = (socket_family == AF_INET6) ? sizeof(addr6) : sizeof(addr);
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&(client->addr6.sin6_addr)
            : (void *)&(client->addr.sin_addr),
        str_client, sizeof(str_client));

    ReflectorUPacket pack_reflect;
    memset(&pack_reflect, 0, sizeof(pack_reflect));
    SenderUPacket pack;
    memset(&pack, 0, sizeof(pack));

    struct msghdr *message = malloc(sizeof(struct msghdr));
    struct iovec *iov = malloc(sizeof(struct iovec));
    char *ctrl_buf = malloc(TST_PKT_SIZE);
    if (!message || !iov || !ctrl_buf) {
        free(message); free(iov); free(ctrl_buf);
        return -1;
    }

    memset(message, 0, sizeof(*message));
    message->msg_name = (socket_family == AF_INET6) ? (void *)&addr6 : (void *)&addr;
    message->msg_namelen = len;
    iov->iov_base = &pack;
    iov->iov_len = TST_PKT_SIZE;
    message->msg_iov = iov;
    message->msg_iovlen = 1;
#ifndef NO_MESSAGE_CONTROL
    message->msg_control = ctrl_buf;
    message->msg_controllen = TST_PKT_SIZE;
#endif

    int rv = recvmsg(client->sessions[session_index].socket, message, 0);
    pack_reflect.receive_time = get_timestamp();

    char str_sender[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
        (socket_family == AF_INET6)
            ? (void *)&addr6.sin6_addr
            : (void *)&addr.sin_addr,
        str_sender, sizeof(str_sender));

    if (rv <= 0) { fprintf(stderr, "[%s] Failed to receive TWAMP-Test\n", str_sender); goto cleanup; }
    if (rv < 14) { fprintf(stderr, "[%s] Short packet (%d bytes)\n", str_sender, rv); goto cleanup; }

    uint8_t fw_ttl = 0, fw_tos = 0;
    struct cmsghdr *c_msg;
#ifndef NO_MESSAGE_CONTROL
    for (c_msg = CMSG_FIRSTHDR(message); c_msg; c_msg = CMSG_NXTHDR(message, c_msg)) {
        if ((c_msg->cmsg_level == IPPROTO_IP && c_msg->cmsg_type == IP_TTL) ||
            (c_msg->cmsg_level == IPPROTO_IPV6 && c_msg->cmsg_type == IPV6_HOPLIMIT))
            fw_ttl = *(int *)CMSG_DATA(c_msg);
        else if (c_msg->cmsg_level == IPPROTO_IP && c_msg->cmsg_type == IP_TOS)
            fw_tos = *(int *)CMSG_DATA(c_msg);
    }
#endif

    pack_reflect.seq_number = htonl(client->sessions[session_index].seq_nb++);
    pack_reflect.error_estimate = htons(0x8001);
    pack_reflect.sender_seq_number = pack.seq_number;
    pack_reflect.sender_time = pack.time;
    pack_reflect.sender_error_estimate = pack.error_estimate;
    pack_reflect.sender_ttl = fw_ttl;
    if ((client->mode & kModeDSCPECN) == kModeDSCPECN)
        pack_reflect.sender_tos = fw_tos;

    if (client->sessions[session_index].fw_msg == 0) {
        client->sessions[session_index].fw_msg = 1;
        if ((fw_tos & 0x03) > 0) {
            uint8_t ecn_tos = (fw_tos & 0x03) - (((fw_tos & 0x2) >> 1) & (fw_tos & 0x1));
            set_socket_tos(client->sessions[session_index].socket,
                (client->sessions[session_index].req.TypePDescriptor << 2) + ecn_tos);
        }
    } else {
        client->sessions[session_index].fw_msg +=
            ntohl(pack.seq_number) - client->sessions[session_index].snd_nb;
        client->sessions[session_index].fw_lst_msg +=
            ntohl(pack.seq_number) - client->sessions[session_index].snd_nb - 1;
    }
    client->sessions[session_index].snd_nb = ntohl(pack.seq_number);
    pack_reflect.time = get_timestamp();

    {
        int send_len = (rv < 41) ? 41 : rv;
        if (socket_family == AF_INET6)
            rv = sendto(client->sessions[session_index].socket, &pack_reflect, send_len, 0,
                (struct sockaddr *)&addr6, sizeof(addr6));
        else
            rv = sendto(client->sessions[session_index].socket, &pack_reflect, send_len, 0,
                (struct sockaddr *)&addr, sizeof(addr));
    }

    if (rv <= 0) fprintf(stderr, "[%s] Failed to send TWAMP-Test reply\n", str_client);

    print_metrics_server(str_client,
        socket_family == AF_INET6 ? ntohs(addr6.sin6_port) : ntohs(addr.sin_port),
        ntohs(client->sessions[session_index].req.ReceiverPort),
        client->sessions[session_index].req.TypePDescriptor << 2,
        fw_tos, &pack_reflect);

    if ((client->sessions[session_index].fw_msg % 10) == 0)
        printf("FW Lost: %u/%u (%.2f%%)\n",
            client->sessions[session_index].fw_lst_msg,
            client->sessions[session_index].fw_msg,
            (float)100 * client->sessions[session_index].fw_lst_msg /
                client->sessions[session_index].fw_msg);

cleanup:
    free(iov); free(message); free(ctrl_buf);
    return rv;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    char *progname = (strrchr(argv[0], '/')) ? strrchr(argv[0], '/') + 1 : argv[0];

    if (getuid() == 0) {
        fprintf(stderr, "%s should not be run as root\n", progname);
        exit(EXIT_FAILURE);
    }

    if (parse_options(progname, argc, argv)) {
        usage(progname);
        exit(EXIT_FAILURE);
    }

    TWAMPTimestamp StartTime = get_timestamp();
    int listenfd = socket(socket_family, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("Error opening TCP control socket"); exit(EXIT_FAILURE); }

    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    /* Also enable SO_REUSEPORT so the listening socket can rebind quickly */
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    if (socket_family == AF_INET6) {
        struct sockaddr_in6 serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin6_family = AF_INET6;
        serv_addr.sin6_addr = in6addr_any;
        serv_addr.sin6_port = htons(tcp_port);
        if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("bind IPv6"); close(listenfd); exit(EXIT_FAILURE);
        }
    } else {
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port = htons(tcp_port);
        if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr)) < 0) {
            perror("bind IPv4"); close(listenfd); exit(EXIT_FAILURE);
        }
    }

    used_sockets++;
    if (listen(listenfd, MAX_CLIENTS) < 0) {
        perror("listen"); close(listenfd); exit(EXIT_FAILURE);
    }
    fprintf(stderr, "TWAMP server listening on TCP port %d\n", tcp_port);
    fprintf(stderr, "UDP test range [%d-%d], max clients=%d\n", port_min, port_max, MAX_CLIENTS);

    FD_ZERO(&read_fds);
    FD_SET(listenfd, &read_fds);
    fd_max = listenfd;

    struct client_info clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));

    struct sockaddr_in  client_addr;
    struct sockaddr_in6 client_addr6;
    fd_set tmp_fds;
    FD_ZERO(&tmp_fds);

    /* Use a 1-second timeout so we can enforce idle timeouts */
    struct timeval select_timeout;

    while (1) {
        tmp_fds = read_fds;
        select_timeout.tv_sec = 1;
        select_timeout.tv_usec = 0;

        int sel = select(fd_max + 1, &tmp_fds, NULL, NULL, &select_timeout);
        if (sel < 0) {
            perror("select");
            close(listenfd);
            exit(EXIT_FAILURE);
        }

        /* ---- Handle new connections ---- */
        if (FD_ISSET(listenfd, &tmp_fds)) {
            uint32_t client_len = (socket_family == AF_INET6)
                ? sizeof(client_addr6) : sizeof(client_addr);
            int newsockfd = accept(listenfd,
                (socket_family == AF_INET6)
                    ? (struct sockaddr *)&client_addr6
                    : (struct sockaddr *)&client_addr,
                &client_len);
            if (newsockfd < 0) {
                perror("accept");
            } else {
                int pos = find_empty_client(clients, MAX_CLIENTS);
                if (pos != -1) {
                    /* Enable keepalive to detect dead connections */
                    int yes = 1;
                    setsockopt(newsockfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                    setsockopt(newsockfd, IPPROTO_TCP, TCP_NODELAY,  &yes, sizeof(yes));

                    clients[pos].status   = kConnected;
                    clients[pos].socket   = newsockfd;
                    clients[pos].addr     = client_addr;
                    clients[pos].addr6    = client_addr6;
                    clients[pos].mode     = authmode;
                    clients[pos].sess_no  = 0;
                    gettimeofday(&clients[pos].connect_time, NULL);
                    used_sockets++;
                    FD_SET(newsockfd, &read_fds);
                    if (newsockfd > fd_max) fd_max = newsockfd;
                    send_greeting(0x01FF, &clients[pos]);
                } else {
                    fprintf(stderr, "No available client slot (max=%d), rejecting\n", MAX_CLIENTS);
                    close(newsockfd);
                }
            }
        }

        /* ---- Handle control messages ---- */
        uint8_t buffer[4096];
        int i, j;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].status == kOffline) continue;
            if (!FD_ISSET(clients[i].socket, &tmp_fds)) continue;

            int rv;
            switch (clients[i].status) {
            case kConnected:
                rv = receive_greet_response(&clients[i]);
                if (rv > 32)
                    rv = send_start_serv(&clients[i], StartTime);
                else
                    rv = send_start_serv(&clients[i], ZeroT);
                break;

            case kConfigured:
                memset(buffer, 0, sizeof(buffer));
                rv = recv(clients[i].socket, buffer, sizeof(buffer), 0);
                if (rv <= 0) { cleanup_client(&clients[i]); break; }
                switch (buffer[0]) {
                case kStartSessions:
                    rv = receive_start_sessions(&clients[i]);
                    break;
                case kRequestTWSession:
                    rv = receive_request_session(&clients[i], (RequestSession *)buffer);
                    break;
                default:
                    fprintf(stderr, "Unexpected msg 0x%02X in kConfigured\n", buffer[0]);
                    break;
                }
                if (rv <= 0) cleanup_client(&clients[i]);
                break;

            case kTesting:
                memset(buffer, 0, sizeof(buffer));
                rv = recv(clients[i].socket, buffer, sizeof(buffer), 0);
                if (rv <= 0) { cleanup_client(&clients[i]); break; }
                if (buffer[0] == kStopSessions)
                    receive_stop_sessions(&clients[i]);
                break;

            default:
                break;
            }
        }

        /* ---- Handle UDP test sessions & session expiry ---- */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].status != kTesting) continue;
            struct timeval current;
            gettimeofday(&current, NULL);
            uint8_t has_active = 0;

            for (j = 0; j < clients[i].sess_no; j++) {
                if (clients[i].sessions[j].socket <= 0) continue;
                int alive = get_actual_shutdown(&current, &clients[i].shutdown_time,
                    &clients[i].sessions[j].req.Timeout);
                if (alive > 0) {
                    has_active = 1;
                    if (FD_ISSET(clients[i].sessions[j].socket, &tmp_fds))
                        receive_test_message(&clients[i], j);
                } else {
                    fprintf(stderr, "Session %d ended — FW Lost: %u/%u (%.2f%%)\n",
                        j,
                        clients[i].sessions[j].fw_lst_msg,
                        clients[i].sessions[j].fw_msg,
                        (clients[i].sessions[j].fw_msg > 0)
                            ? (float)100 * clients[i].sessions[j].fw_lst_msg /
                              clients[i].sessions[j].fw_msg
                            : 0.0f);
                    FD_CLR(clients[i].sessions[j].socket, &read_fds);
                    close(clients[i].sessions[j].socket);
                    clients[i].sessions[j].socket = -1;
                    used_sockets--;
                }
            }

            if (!has_active) {
                fprintf(stderr, "All sessions done for client %d — closing TCP connection\n", i);
                /*
                 * KEY FIX: fully clean up the client so the slot is freed.
                 * Previously the code went back to kConfigured keeping the TCP
                 * socket open forever, exhausting MAX_CLIENTS slots.
                 */
                cleanup_client(&clients[i]);
                recompute_fd_max(listenfd, clients, MAX_CLIENTS);
            }
        }

        /* ---- Enforce idle timeout for stuck kConnected/kConfigured clients ---- */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].status != kConnected && clients[i].status != kConfigured)
                continue;
            struct timeval now;
            gettimeofday(&now, NULL);
            long idle = now.tv_sec - clients[i].connect_time.tv_sec;
            if (idle > CLIENT_IDLE_TIMEOUT) {
                fprintf(stderr, "Client %d idle for %lds, forcing cleanup\n", i, idle);
                cleanup_client(&clients[i]);
                recompute_fd_max(listenfd, clients, MAX_CLIENTS);
            }
        }
    }

    close(listenfd);
    return 0;
}
