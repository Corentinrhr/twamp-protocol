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
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include "twamp.h"
#ifndef IPV6_HOPLIMIT
#define IPV6_HOPLIMIT IPV6_UNICAST_HOPS
#endif

#define MAX_CLIENTS 10
#define MAX_SESSIONS_PER_CLIENT 10
#define PORTBASE_MIN 20000
#define PORTBASE_MAX 30000

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
    uint32_t sid_addr;          /* in network order */
    TWAMPTimestamp sid_time;    /* in network order */
    uint32_t sid_rand;          /* in network order */
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
static uint16_t servo = 0;      /* Server Octets to be sent by sender */
static int socket_family = AF_INET;

TWAMPTimestamp ZeroT = { 0, 0 };

/* Prints the help of the TWAMP server */
static void usage(char *progname)
{
    fprintf(stderr, "Usage: %s [options]\n", progname);
    fprintf(stderr, "\nWhere \"options\" are:\n");

    fprintf(stderr,
            "	-a authmode		Default is Unauthenticated\n"
            "	-p port_min		Port min for Test receivers based on port_min (>1063)\n"
            "	-q port_max		Port max for Test receivers based; at least port_min+1\n"
            "	-c port 		Listening TCP port for TCP control (must not be included between port_min and port_max), default: 862 \n"
            "	-o servo     	2 Octets to be reflected by Sender in Reflected Mode (<65536)\n"
            "	-6              Use IPv6 if this option is defined. Otherwise, Ipv4 will be used.\n"
            "	-h         		Prints this help message and exits\n");
    return;
}

/* Parses the command line arguments for the server */
static int parse_options(char *progname, int argc, char *argv[])
{
    (void)progname;
    int opt;
    /* Removed strict argc check: getopt handles invalid cases itself */

    while ((opt = getopt(argc, argv, "a:p:q:c:o:h6")) != -1) {
        /* 'h' without ':': -h takes no argument */
        switch (opt) {
        case 'a':
            authmode = strtol(optarg, NULL, 10);
            /* For now only unauthenticated mode is supported */
            if (authmode < 0 || authmode > 511)
                return 1;
            break;
        case 'p':
            port_min = atoi(optarg);
            if (port_min < 1024 || port_min > 64535)
                port_min = PORTBASE_MIN;
            /* Do NOT recalculate port_max here: -q must be able to set it independently */
            break;
        case 'q':
            /* port_max validation is done after all options are parsed */
            port_max = atoi(optarg);
            break;
        case 'c':
            tcp_port = atoi(optarg);
            if (tcp_port < 1 || tcp_port > 65535)
                tcp_port = SERVER_PORT;
            /* Conflict between TCP and UDP range is resolved after parsing */
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

    /* Post-parsing validation of the UDP port range */
    if (port_max <= port_min || port_max > 65535) {
        fprintf(stderr,
                "Invalid port range [%d-%d], using defaults [%d-%d]\n",
                port_min, port_max, PORTBASE_MIN, PORTBASE_MAX);
        port_min = PORTBASE_MIN;
        port_max = PORTBASE_MAX;
    }

    /* Ensure the TCP control port does not fall inside the UDP test range */
    if (tcp_port >= port_min && tcp_port <= port_max) {
        fprintf(stderr,
                "TCP port %d conflicts with UDP range [%d-%d], "
                "reverting to default TCP port %d\n",
                tcp_port, port_min, port_max, SERVER_PORT);
        tcp_port = SERVER_PORT;
    }

    fprintf(stderr,
            "Config: TCP control port=%d, UDP test range=[%d-%d]\n",
            tcp_port, port_min, port_max);

    return 0;
}

/* The cleanup_client function will close every connection (TWAMP-Control ot
 * TWAMP-Test that this server has with the client defined by the client_infor
 * structure received as a parameter.
 */
static void cleanup_client(struct client_info *client)
{
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family, (socket_family == AF_INET6) ?
                                    (void*) &(client->addr6.sin6_addr) :
                                    (void*) &(client->addr.sin_addr),
                            str_client, sizeof(str_client));
    fprintf(stderr, "Cleanup client %s\n", str_client);
    //fprintf(stderr, "Cleanup client %s\n", inet_ntoa(client->addr.sin_addr));
    FD_CLR(client->socket, &read_fds);
    close(client->socket);
    used_sockets--;
    int i;
    for (i = 0; i < client->sess_no; i++)
        /* If socket is -1 the session has already been closed */
        if (client->sessions[i].socket > 0) {
            FD_CLR(client->sessions[i].socket, &read_fds);
            close(client->sessions[i].socket);
            client->sessions[i].socket = -1;
            used_sockets--;
        }
    memset(client, 0, sizeof(struct client_info));
    client->status = kOffline;
}

/* The TWAMP server can only accept max_clients and it will recycle the
 * positions for the available clients.
 */
static int find_empty_client(struct client_info *clients, int max_clients)
{
    int i;
    for (i = 0; i < max_clients; i++)
        if (clients[i].status == kOffline)
            return i;
    return -1;
}

/* Sends a ServerGreeting message to the Control-Client after
 * the TCP connection has been established.
 */
static int send_greeting(uint16_t mode_mask, struct client_info *client)
{
    int socket = client->socket;

    char str_client[INET6_ADDRSTRLEN];   /* String for Client IP address */
    inet_ntop(socket_family, (socket_family == AF_INET6)?
                                   (void*) &(client->addr6.sin6_addr) :
                                   (void*) &(client->addr.sin_addr),
                            str_client, sizeof(str_client));

    int i;
    ServerGreeting greet;
    memset(&greet, 0, sizeof(greet));
    greet.Modes = htonl(client->mode & mode_mask);
    for (i = 0; i < 16; i++)
        greet.Challenge[i] = rand() % 16;
    for (i = 0; i < 16; i++)
        greet.Salt[i] = rand() % 16;
    greet.Count = htonl(1 << 10);

    int rv = send(socket, &greet, sizeof(greet), 0);
    if (rv < 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to send ServerGreeting message");
        cleanup_client(client);
    } else if ((authmode & 0x000F) == 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Sent ServerGreeting message with Mode 0! Abort");
        cleanup_client(client);
    } else {
        printf("Sent ServerGreeting message to %s\n", str_client);
    }
    return rv;
}

/* After a ServerGreeting the Control-Client should respond with a
 * SetUpResponse. This function treats this message
 */
static int receive_greet_response(struct client_info *client)
{
    int socket = client->socket;
    char str_client[INET6_ADDRSTRLEN];   /* String for Client IP address */
    inet_ntop(socket_family, (socket_family == AF_INET6)?
                                   (void*) &(client->addr6.sin6_addr) :
                                   (void*) &(client->addr.sin_addr),
                            str_client, sizeof(str_client));

    SetUpResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rv = recv(socket, &resp, sizeof(resp), 0);
    if (rv <= 32) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to receive SetUpResponse");
        //client->mode = ntohl(0);
        cleanup_client(client);
    } else {
        fprintf(stderr, "Received SetUpResponse message from %s with mode %d\n",
                str_client, ntohl(resp.Mode));
        if ((ntohl(resp.Mode) & client->mode & 0x000F) == 0) {
            perror("The client does not support any usable Mode");
            rv = 0;
        }
        client->mode = ntohl(resp.Mode);
    }
    return rv;
}

/* Sent a ServerStart message to the Control-Client to end
 * the TWAMP-Control session establishment phase
 */
static int send_start_serv(struct client_info *client, TWAMPTimestamp StartTime)
{
    int socket = client->socket;

    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family, (socket_family == AF_INET6)?
                                   (void*) &(client->addr6.sin6_addr) :
                                   (void*) &(client->addr.sin_addr),
                            str_client, sizeof(str_client));

    ServerStart msg;
    memset(&msg, 0, sizeof(msg));
    if ((StartTime.integer == 0) && (StartTime.fractional == 0)) {
        msg.Accept = kAspectNotSupported;
    } else {
        msg.Accept = kOK;
    }
    msg.StartTime = StartTime;
    int rv = send(socket, &msg, sizeof(msg), 0);
    if (rv <= 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to send ServerStart message");
        cleanup_client(client);
    } else {
        client->status = kConfigured;
        printf("ServerStart message sent to %s\n", str_client);
        if (msg.Accept == kAspectNotSupported) {
            cleanup_client(client);
        }
    }
    return rv;
}

/* Sends a StartACK for the StartSessions message */
static int send_start_ack(struct client_info *client)
{
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family, (socket_family == AF_INET6)?
                                   (void*) &(client->addr6.sin6_addr) :
                                   (void*) &(client->addr.sin_addr),
                            str_client, sizeof(str_client));
    StartACK ack;
    memset(&ack, 0, sizeof(ack));
    ack.Accept = kOK;
    int rv = send(client->socket, &ack, sizeof(ack), 0);
    if (rv <= 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to send StartACK message");
    } else
        printf("StartACK message sent to %s\n", str_client);
    return rv;
}

/* This function treats the case when a StartSessions is received from the
 * Control-Client to start a number of TWAMP-Test sessions
 */
static int receive_start_sessions(struct client_info *client)
{
    int i;
    int rv = send_start_ack(client);
    if (rv <= 0)
        return rv;

    /* Now it can receive packets on the TWAMP-Test sockets */
    for (i = 0; i < client->sess_no; i++) {
        FD_SET(client->sessions[i].socket, &read_fds);
        if (fd_max < client->sessions[i].socket)
            fd_max = client->sessions[i].socket;
    }
    client->status = kTesting;
    /* Title for printing */
    fprintf(stderr,
            "\tSnd@\t,\tTime\t, Snd#\t, Rcv#\t, SndPt\t,"
            " RcvPt\t,  Sync\t, TTL\t, SndTOS, FW_TOS, Int D\t," " FWD [ms]\n");
    return rv;
}

/* This functions treats the case when a StopSessions is received from
 * the Control-Client to end all the Test sessions.
 */
static int receive_stop_sessions(struct client_info *client)
{
    /* If a StopSessions message was received, it can still receive Test packets
     * until the timeout has expired */
    gettimeofday(&client->shutdown_time, NULL);
    return 0;
}

/* Computes the response to a RequestTWSession message */
static int send_accept_session(struct client_info *client, RequestSession *req)
{
    char str_client[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
              (socket_family == AF_INET6) ?
                  (void *)&(client->addr6.sin6_addr) :
                  (void *)&(client->addr.sin_addr),
              str_client, sizeof(str_client));

    AcceptSession acc;
    memset(&acc, 0, sizeof(acc));

    if ((used_sockets < 64) && (client->sess_no < MAX_SESSIONS_PER_CLIENT)) {

        int testfd = socket(socket_family, SOCK_DGRAM, 0);
        if (testfd < 0) {
            fprintf(stderr, "[%s] ", str_client);
            perror("Error opening UDP test socket");
            return -1;
        }

        /* +1 to make port_max inclusive in the random selection */
        int port_range = port_max - port_min + 1;
        int check_time = CHECK_TIMES;

        if (socket_family == AF_INET6) {
            struct sockaddr_in6 local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin6_family = AF_INET6;
            local_addr.sin6_addr   = in6addr_any;
            /* Server selects the UDP port from its own range,
             * ignoring req->ReceiverPort requested by the client.
             * The chosen port is returned to the client in Accept-Session.Port. */
            local_addr.sin6_port   = htons(port_min + rand() % port_range);

            while (check_time-- &&
                   bind(testfd, (struct sockaddr *)&local_addr,
                        sizeof(local_addr)) < 0)
                local_addr.sin6_port = htons(port_min + rand() % port_range);

            /* >= 0 covers success on the last attempt (check_time was decremented) */
            if (check_time >= 0)
                req->ReceiverPort = local_addr.sin6_port;

        } else {
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family      = AF_INET;
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            /* Same logic as IPv6 block above */
            local_addr.sin_port        = htons(port_min + rand() % port_range);

            while (check_time-- &&
                   bind(testfd, (struct sockaddr *)&local_addr,
                        sizeof(struct sockaddr)) < 0)
                local_addr.sin_port = htons(port_min + rand() % port_range);

            if (check_time >= 0)
                req->ReceiverPort = local_addr.sin_port;
        }

        if (check_time >= 0) {

            acc.Accept = kOK;
            acc.Port   = req->ReceiverPort;
            client->sessions[client->sess_no].socket = testfd;
            client->sessions[client->sess_no].req    = *req;

            /* SID construction */
            memcpy(acc.SID, &req->ReceiverAddress, 4);
            TWAMPTimestamp sidtime = get_timestamp();
            memcpy(&acc.SID[4], &sidtime, 8);
            int k;
            for (k = 0; k < 4; k++)
                acc.SID[12 + k] = rand() % 256;
            memcpy(&client->sessions[client->sess_no].sid_addr,  &acc.SID,      4);
            memcpy(&client->sessions[client->sess_no].sid_rand,  &acc.SID[12],  4);
            client->sessions[client->sess_no].sid_time = sidtime;

            /* Reflect Octets mode */
            if ((client->mode & kModeReflectOctets) == kModeReflectOctets) {
                acc.ReflectedOctets = req->OctetsToBeReflected;
                client->sessions[client->sess_no].server_oct = servo;
                acc.ServerOctets = htons(servo);
                printf("Reflected Octets: %u, Server Octets: %u\n",
                       ntohs(acc.ReflectedOctets),
                       client->sessions[client->sess_no].server_oct);
            }

            fprintf(stderr,
                    "SID: 0x%04X.%04X.%04X.%04X  UDP port: %d\n",
                    ntohl(client->sessions[client->sess_no].sid_addr),
                    ntohl(client->sessions[client->sess_no].sid_time.integer),
                    ntohl(client->sessions[client->sess_no].sid_time.fractional),
                    ntohl(client->sessions[client->sess_no].sid_rand),
                    ntohs(req->ReceiverPort));

            set_socket_option(testfd, HDR_TTL);
            set_socket_tos(testfd,
                           client->sessions[client->sess_no].req.TypePDescriptor << 2);

            client->sess_no++;
            /* FIX: count the newly created UDP test socket */
            used_sockets++;

        } else {
            /* FIX: close testfd on bind failure to avoid file descriptor leak */
            close(testfd);
            fprintf(stderr,
                    "[%s] kTemporaryResourceLimitation: "
                    "bind failed after %d tries in range [%d-%d]\n",
                    str_client, CHECK_TIMES, port_min, port_max);
            acc.Accept = kTemporaryResourceLimitation;
            acc.Port   = 0;
        }

    } else {
        fprintf(stderr,
                "[%s] kTemporaryResourceLimitation: "
                "used_sockets=%d, sess_no=%d\n",
                str_client, used_sockets, client->sess_no);
        acc.Accept = kTemporaryResourceLimitation;
        acc.Port   = 0;
    }

    int rv = send(client->socket, &acc, sizeof(acc), 0);
    return rv;
}

/* This function treats the case when a RequestTWSession is received */
static int receive_request_session(struct client_info *client,
                                   RequestSession * req)
{
    char str_client[INET6_ADDRSTRLEN];   /* String for Client IP address */

    if(socket_family == AF_INET6) {
        inet_ntop(AF_INET6, &(client->addr6.sin6_addr), str_client, sizeof(str_client));
        fprintf(stderr, "Server received RequestTWSession message\n");
    } else {
        char str_server[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client->addr.sin_addr), str_client, INET_ADDRSTRLEN);
        struct in_addr se_addr;
        se_addr.s_addr = req->ReceiverAddress;
        inet_ntop(AF_INET, &(se_addr), str_server, INET_ADDRSTRLEN);
        fprintf(stderr, "Server %s received RequestTWSession message\n", str_server);
    }

    int rv = send_accept_session(client, req);
    if (rv <= 0) {
        fprintf(stderr, "[%s] ", str_client);
        perror("Failed to send the Accept-Session message");
    }
    return rv;
}

/* This function will receive a TWAMP-Test packet and will send a response. In
 * TWAMP the Session-Sender (in our case the Control-Client, meaning the
 * TWAMP-Client) is always sending TWAMP-Test packets and the Session-Reflector
 * (Server) is receiving TWAMP-Test packets.
 */

static int receive_test_message(struct client_info *client, int session_index)
{
    struct sockaddr_in  addr;
    struct sockaddr_in6 addr6;
    socklen_t len = (socket_family == AF_INET6) ? sizeof(addr6) : sizeof(addr);
    char str_client[INET6_ADDRSTRLEN];

    inet_ntop(socket_family,
              (socket_family == AF_INET6) ?
                  (void *)&(client->addr6.sin6_addr) :
                  (void *)&(client->addr.sin_addr),
              str_client, sizeof(str_client));

    ReflectorUPacket pack_reflect;
    memset(&pack_reflect, 0, sizeof(pack_reflect));

    SenderUPacket pack;
    memset(&pack, 0, sizeof(pack));

    /* Heap-allocate ancillary data structures so we can free them on every
     * exit path via a single goto label */
    struct msghdr *message      = malloc(sizeof(struct msghdr));
    struct iovec  *iov          = malloc(sizeof(struct iovec));
    char          *ctrl_buf     = malloc(TST_PKT_SIZE);

    if (!message || !iov || !ctrl_buf) {
        fprintf(stderr, "[%s] malloc failed in receive_test_message\n",
                str_client);
        free(message);
        free(iov);
        free(ctrl_buf);
        return -1;
    }

    memset(message, 0, sizeof(*message));
    message->msg_name    = (socket_family == AF_INET6) ?
                               (void *)&addr6 : (void *)&addr;
    message->msg_namelen = len;
    iov->iov_base        = &pack;
    iov->iov_len         = TST_PKT_SIZE;
    message->msg_iov     = iov;
    message->msg_iovlen  = 1;
#ifndef NO_MESSAGE_CONTROL
    message->msg_control    = ctrl_buf;
    message->msg_controllen = TST_PKT_SIZE;
#endif

    int rv = recvmsg(client->sessions[session_index].socket, message, 0);

    /* Capture receive timestamp as early as possible */
    pack_reflect.receive_time = get_timestamp();

    char str_sender[INET6_ADDRSTRLEN];
    inet_ntop(socket_family,
              (socket_family == AF_INET6) ?
                  (void *)&addr6.sin6_addr :
                  (void *)&addr.sin_addr,
              str_sender, sizeof(str_sender));

    if (rv <= 0) {
        fprintf(stderr, "[%s] Failed to receive TWAMP-Test packet\n",
                str_sender);
        goto cleanup;
    }
    if (rv < 14) {
        fprintf(stderr, "[%s] Short TWAMP-Test packet (%d bytes)\n",
                str_sender, rv);
        goto cleanup;
    }

    /* Extract TTL and TOS from the IP header via ancillary (cmsg) data */
    uint8_t fw_ttl = 0;
    uint8_t fw_tos = 0;
    struct cmsghdr *c_msg;

#ifndef NO_MESSAGE_CONTROL
    for (c_msg = CMSG_FIRSTHDR(message); c_msg;
         c_msg = CMSG_NXTHDR(message, c_msg)) {
        if ((c_msg->cmsg_level == IPPROTO_IP   && c_msg->cmsg_type == IP_TTL)
         || (c_msg->cmsg_level == IPPROTO_IPV6 &&
             c_msg->cmsg_type  == IPV6_HOPLIMIT)) {
            fw_ttl = *(int *)CMSG_DATA(c_msg);
        } else if (c_msg->cmsg_level == IPPROTO_IP &&
                   c_msg->cmsg_type  == IP_TOS) {
            fw_tos = *(int *)CMSG_DATA(c_msg);
        } else {
            fprintf(stderr,
                    "\tWarning: unexpected ancillary data "
                    "level=%d type=%d\n",
                    c_msg->cmsg_level, c_msg->cmsg_type);
        }
    }
#else
    fprintf(stdout,
            "No message control on this platform, "
            "cannot retrieve IP header options\n");
#endif

    /* Build the reflected packet */
    pack_reflect.seq_number            = htonl(client->sessions[session_index].seq_nb++);
    pack_reflect.error_estimate        = htons(0x8001); /* Sync=1, Multiplier=1 */
    pack_reflect.sender_seq_number     = pack.seq_number;
    pack_reflect.sender_time           = pack.time;
    pack_reflect.sender_error_estimate = pack.error_estimate;
    pack_reflect.sender_ttl            = fw_ttl;
    if ((client->mode & kModeDSCPECN) == kModeDSCPECN)
        pack_reflect.sender_tos = fw_tos;

    /*
     * Reply destination: use the real source address and port observed in
     * the received packet (already stored in addr/addr6 by recvmsg).
     * This is more robust than forcing req.SenderPort when NAT is involved.
     *
     * If your deployment has no NAT and you need strict TWAMP behaviour,
     * replace with:
     *   addr.sin_port  = client->sessions[session_index].req.SenderPort;
     * FIX: the original code called inet_ntoa() on a port value, which is
     * both a type mismatch and logically wrong — removed entirely.
     */

    /* Forward loss counters */
    if (client->sessions[session_index].fw_msg == 0) {
        client->sessions[session_index].fw_msg = 1;
        /* Handle ECN bits in TOS for the first packet */
        if ((fw_tos & 0x03) > 0) {
            uint8_t ecn_tos = (fw_tos & 0x03) -
                              (((fw_tos & 0x2) >> 1) & (fw_tos & 0x1));
            set_socket_tos(client->sessions[session_index].socket,
                           (client->sessions[session_index].req.TypePDescriptor
                            << 2) + ecn_tos);
        }
    } else {
        client->sessions[session_index].fw_msg +=
            ntohl(pack.seq_number) - client->sessions[session_index].snd_nb;
        client->sessions[session_index].fw_lst_msg +=
            ntohl(pack.seq_number) -
            client->sessions[session_index].snd_nb - 1;
    }
    client->sessions[session_index].snd_nb = ntohl(pack.seq_number);

    /* Capture send timestamp just before sending */
    pack_reflect.time = get_timestamp();

    /* Send reply — minimum 41 bytes per RFC 5357 */
    {
        int send_len = (rv < 41) ? 41 : rv;
        if (socket_family == AF_INET6)
            rv = sendto(client->sessions[session_index].socket,
                        &pack_reflect, send_len, 0,
                        (struct sockaddr *)&addr6, sizeof(addr6));
        else
            rv = sendto(client->sessions[session_index].socket,
                        &pack_reflect, send_len, 0,
                        (struct sockaddr *)&addr, sizeof(addr));
    }

    if (rv <= 0)
        fprintf(stderr, "[%s] Failed to send TWAMP-Test reply\n", str_client);

    print_metrics_server(
        str_client,
        socket_family == AF_INET6 ? ntohs(addr6.sin6_port) : ntohs(addr.sin_port),
        ntohs(client->sessions[session_index].req.ReceiverPort),
        client->sessions[session_index].req.TypePDescriptor << 2,
        fw_tos,
        &pack_reflect);

    if ((client->sessions[session_index].fw_msg % 10) == 0)
        printf("FW Lost packets: %u/%u, FW Loss Ratio: %3.2f%%\n",
               client->sessions[session_index].fw_lst_msg,
               client->sessions[session_index].fw_msg,
               (float)100 *
               client->sessions[session_index].fw_lst_msg /
               client->sessions[session_index].fw_msg);

cleanup:
    /* FIX: always free heap buffers, including on early returns */
    free(iov);
    free(message);
    free(ctrl_buf);
    return rv;
}

int main(int argc, char *argv[])
{
    char *progname = NULL;
    srand(time(NULL));

    /* Extract the program name without the full path */
    progname = (progname == strrchr(argv[0], '/')) ? progname + 1 : *argv;

#if 1
    /* Safety check: this server should not run as root */
    if (getuid() == 0) {
        fprintf(stderr, "%s should not be run as root\n", progname);
        exit(EXIT_FAILURE);
    }
#endif

    /* Parse command-line options */
    if (parse_options(progname, argc, argv)) {
        usage(progname);
        exit(EXIT_FAILURE);
    }

    /* Record server start time in TWAMP timestamp format */
    TWAMPTimestamp StartTime = get_timestamp();

    /* Create the TCP control socket */
    int listenfd = socket(socket_family, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("Error opening TCP control socket");
        exit(EXIT_FAILURE);
    }

    /* Allow fast socket reuse after restart */
    int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
    }

    /* Bind the TCP control socket to the configured port */
    if (socket_family == AF_INET6) {
        struct sockaddr_in6 serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin6_family = AF_INET6;
        serv_addr.sin6_addr   = in6addr_any;
        serv_addr.sin6_port   = htons(tcp_port);

        if (bind(listenfd, (struct sockaddr *)&serv_addr,
                 sizeof(serv_addr)) < 0) {
            perror("Error on binding TCP control socket (IPv6)");
            close(listenfd);
            exit(EXIT_FAILURE);
        }
    } else {
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family      = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port        = htons(tcp_port);

        if (bind(listenfd, (struct sockaddr *)&serv_addr,
                 sizeof(struct sockaddr)) < 0) {
            perror("Error on binding TCP control socket (IPv4)");
            close(listenfd);
            exit(EXIT_FAILURE);
        }
    }

    used_sockets++;

    // Start listening for incoming TWAMP-Control connections
    if (listen(listenfd, MAX_CLIENTS) < 0) {
        perror("Error on listen");
        close(listenfd);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "TWAMP server listening on TCP port %d\n", tcp_port);
    fprintf(stderr, "UDP test sessions will use port range [%d-%d]\n",
            port_min, port_max);

    FD_ZERO(&read_fds);
    FD_SET(listenfd, &read_fds);
    fd_max = listenfd;

    /* Initialise the client table */
    struct client_info clients[MAX_CLIENTS];
    memset(clients, 0, MAX_CLIENTS * sizeof(struct client_info));

    int newsockfd;
    struct sockaddr_in  client_addr;
    struct sockaddr_in6 client_addr6;
    fd_set tmp_fds;
    FD_ZERO(&tmp_fds);

    int rv;

    while (1) {
        tmp_fds = read_fds;

        if (select(fd_max + 1, &tmp_fds, NULL, NULL, NULL) < 0) {
            perror("Error in select");
            close(listenfd);
            exit(EXIT_FAILURE);
        }
        
        // New TWAMP-Control TCP connection
        if (FD_ISSET(listenfd, &tmp_fds)) {
            uint32_t client_len = (socket_family == AF_INET6) ?
                                  sizeof(client_addr6) : sizeof(client_addr);

            newsockfd = accept(listenfd,
                               (socket_family == AF_INET6) ?
                                   (struct sockaddr *)&client_addr6 :
                                   (struct sockaddr *)&client_addr,
                               &client_len);

            if (newsockfd < 0) {
                perror("Error in accept");
            } else {
                int pos = find_empty_client(clients, MAX_CLIENTS);

                if (pos != -1) {
                    clients[pos].status  = kConnected;
                    clients[pos].socket  = newsockfd;
                    clients[pos].addr    = client_addr;
                    clients[pos].addr6   = client_addr6;
                    clients[pos].mode    = authmode;
                    clients[pos].sess_no = 0;
                    used_sockets++;
                    FD_SET(newsockfd, &read_fds);
                    if (newsockfd > fd_max)
                        fd_max = newsockfd;

                    /* FIX: send_greeting is now inside the pos != -1 block,
                     * preventing an out-of-bounds access to clients[-1] */
                    rv = send_greeting(0x01FF, &clients[pos]);
                } else {
                    /* No available slot: reject the connection cleanly */
                    fprintf(stderr,
                            "No available client slot, "
                            "rejecting new connection\n");
                    close(newsockfd);
                }
            }
        }

        /* TWAMP-Control messages from established sessions                   */
        uint8_t buffer[4096];
        int i, j;

        for (i = 0; i < MAX_CLIENTS; i++) {
            /* Skip clients that are offline */
            if (clients[i].status == kOffline)
                continue;

            if (!FD_ISSET(clients[i].socket, &tmp_fds))
                continue;

            switch (clients[i].status) {

            case kConnected:
                /* A TCP session is established and a ServerGreeting has been
                 * sent. Now wait for the SetUpResponse to complete the
                 * TWAMP-Control handshake. */
                rv = receive_greet_response(&clients[i]);
                if (rv > 32)
                    rv = send_start_serv(&clients[i], StartTime);
                else
                    rv = send_start_serv(&clients[i], ZeroT);
                break;

            case kConfigured:
                /* The session is configured. Accept either a RequestTWSession
                 * or a StartSessions message. */
                memset(buffer, 0, sizeof(buffer));
                rv = recv(clients[i].socket, buffer, sizeof(buffer), 0);
                if (rv <= 0) {
                    cleanup_client(&clients[i]);
                    break;
                }

                switch (buffer[0]) {
                case kStartSessions:
                    rv = receive_start_sessions(&clients[i]);
                    break;
                case kRequestTWSession:
                    rv = receive_request_session(&clients[i],
                                                 (RequestSession *)buffer);
                    break;
                default:
                    fprintf(stderr,
                            "Unexpected message type 0x%02X in kConfigured "
                            "state\n", buffer[0]);
                    break;
                }

                if (rv <= 0)
                    cleanup_client(&clients[i]);
                break;

            case kTesting:
                /* In this state only a StopSessions message is expected on
                 * the control socket. */
                memset(buffer, 0, sizeof(buffer));
                rv = recv(clients[i].socket, buffer, sizeof(buffer), 0);
                if (rv <= 0) {
                    cleanup_client(&clients[i]);
                    break;
                }
                if (buffer[0] == kStopSessions)
                    rv = receive_stop_sessions(&clients[i]);
                break;

            default:
                break;
            }
        }

        // TWAMP-Test UDP packets
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].status != kTesting)
                continue;

            struct timeval current;
            gettimeofday(&current, NULL);

            uint8_t has_active_test_sessions = 0;

            for (j = 0; j < clients[i].sess_no; j++) {
                rv = get_actual_shutdown(&current,
                                         &clients[i].shutdown_time,
                                         &clients[i].sessions[j].req.Timeout);

                if (rv > 0) {
                    /* Session is still within its timeout window */
                    has_active_test_sessions = 1;

                    if (FD_ISSET(clients[i].sessions[j].socket, &tmp_fds))
                        rv = receive_test_message(&clients[i], j);

                } else {
                    /* Session has expired: close the UDP test socket */
                    FD_CLR(clients[i].sessions[j].socket, &read_fds);
                    close(clients[i].sessions[j].socket);
                    clients[i].sessions[j].socket = -1;
                    used_sockets--;

                    /* Print final per-session loss statistics */
                    fprintf(stderr,
                            "Session %d ended — FW Lost: %u/%u, "
                            "FW Loss Ratio: %3.2f%%\n",
                            j,
                            clients[i].sessions[j].fw_lst_msg,
                            clients[i].sessions[j].fw_msg,
                            (clients[i].sessions[j].fw_msg > 0) ?
                                (float)100 *
                                clients[i].sessions[j].fw_lst_msg /
                                clients[i].sessions[j].fw_msg : 0.0f);
                }
            }

            /* If all test sessions for this client are done, go back to
             * kConfigured so it can request new sessions */
            if (!has_active_test_sessions) {
                memset(&clients[i].shutdown_time, 0,
                       sizeof(clients[i].shutdown_time));
                clients[i].sess_no = 0;
                clients[i].status  = kConfigured;
                fprintf(stderr,
                        "All test sessions closed for client %d, "
                        "back to kConfigured\n", i);
            }
        }

    } /* end while(1) */

    /* Unreachable in normal operation */
    close(listenfd);
    return 0;
}
