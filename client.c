/*
 * Name: Emma Mirică
 * Project: TWAMP Protocol
 * Class: OSS
 * Email: emma.mirica@cti.pub.ro
 * Contributions: stephanDB
 *
 * Source: client.c
 * Note: contains the TWAMP client implementation.
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
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <errno.h>
#include "twamp.h"

#define PORTBASE_SEND    30000
#define PORTBASE_RECV    20000
#define TEST_SESSIONS    1
#define TEST_MESSAGES    1
#define TIMEOUT          10     /* SECONDS - Timeout for TWAMP test packet
                                   Loss Threshold - in TWAMP-C Request Session */
#define STTIME           0      /* SECONDS - Time to start the Test session
                                   in TWAMP-C Request Session */
#define WAIT             1      /* SECONDS - Waiting time before Test session */
#ifndef IPV6_HOPLIMIT
#define IPV6_HOPLIMIT IPV6_UNICAST_HOPS
#endif

struct twamp_test_info {
    int testfd;
    uint16_t testport;
    uint16_t port;
    uint16_t serveroct;         /* in network order */
};

const uint8_t One = 1;
static enum Mode authmode = kModeUnauthenticated;
static long port_send = PORTBASE_SEND;
static long port_recv = PORTBASE_RECV;
static uint16_t test_sessions_no = TEST_SESSIONS;
static uint32_t test_sessions_msg = TEST_MESSAGES;
static uint64_t interv_msg = 0;
static uint8_t dscp_snd = 0;
static uint16_t payload_len = 160;
static uint8_t mbz_offset = 0;  /* Offset for padding in Symmetrical and DSCP/ECN Modes */
static uint16_t active_sessions = 0;
static uint16_t otbr = 0;       /* Octets To Be Reflected */
static int socket_family = AF_INET;

static enum Mode workmode = kModeUnauthenticated;

static uint8_t snd_tos = 0;     /* IP TOS=0 value in TWAMP */

/* The function that prints the help for this program */
static void usage(char *progname)
{
    fprintf(stderr, "Usage: %s [options]\n", progname);
    fprintf(stderr, "\nWhere \"options\" are:\n");
    fprintf(stderr,
            "   -s  server      The TWAMP server IP [Mandatory]\n"
            "   -a  authmode    Default Unauthenticated\n"
            "   -p  port_sender Minimum sender-side UDP port (>1023)\n"
            "   -P  port_recv   Hint for receiver-side UDP port (>1023) "
                                "(server may override it)\n"
            "   -n  test_sess   Number of Test sessions\n"
            "   -m  no_test_msg Number of Test packets per session\n"
            "   -l  payload_len Length of Test packets in bytes (41..%d)\n"
            "   -t  snd_tos     IP TOS value for Test packets (<256)\n"
            "   -d  snd_dscp    DSCP value for Test packets (<64)\n"
            "   -o  otbr        Octets to be reflected in Reflect mode (<65536)\n"
            "   -i  interval    Interval between Test packets [ms] (<10000)\n"
            "   -c  port        TCP control port of the server (default: %d)\n"
            "   -6              Use IPv6 (default: IPv4)\n"
            "   -h              Print this help and exit\n",
            TST_PKT_SIZE, SERVER_PORT);
}

/* The parse_options will check the command line arguments */
static int parse_options(struct hostent **server, int argc, char *argv[],
                         int *tcp_ctrl_port)
{
    /* Minimum: at least the program name + -s server */
    if (argc < 2)
        return 1;

    char *server_host = NULL;
    int opt;

    /* 'h' without ':' — -h takes no argument */
    while ((opt = getopt(argc, argv, "s:a:p:P:n:m:l:t:d:i:o:c:h6")) != -1) {
        switch (opt) {
        case 's':
            server_host = optarg;
            break;

        case 'a':
            authmode = strtol(optarg, NULL, 10);
            /* Only unauthenticated mode is supported for now */
            if (authmode < 0 || authmode > 511)
                return 1;
            break;

        case 'p':
            port_send = strtol(optarg, NULL, 10);
            if (port_send < 1024 || port_send > 65535)
                return 1;
            break;

        case 'P':
            port_recv = strtol(optarg, NULL, 10);
            if (port_recv < 1024 || port_recv > 65535)
                return 1;
            break;

        case 'n': {
            errno = 0;
            long sessions = strtol(optarg, NULL, 10);
            if (errno == ERANGE || sessions >= INT_MAX || sessions <= 0) {
                perror("strtol");
                return 1;
            }
            test_sessions_no = (uint16_t)sessions;
            break;
        }

        case 'm': {
            errno = 0;
            long msgs = strtol(optarg, NULL, 10);
            if (errno == ERANGE || msgs >= LONG_MAX || msgs <= 0) {
                perror("strtol");
                return 1;
            }
            test_sessions_msg = (uint32_t)msgs;
            break;
        }

        case 'l':
            payload_len = strtol(optarg, NULL, 10);
            if (payload_len < 41 || payload_len > TST_PKT_SIZE)
                return 1;
            break;

        case 't':
            snd_tos = strtol(optarg, NULL, 10);
            /* Clear ECN bits to avoid congestion notification on sent packets */
            snd_tos = snd_tos - (((snd_tos & 0x2) >> 1) & (snd_tos & 0x1));
            break;

        case 'd':
            dscp_snd = strtol(optarg, NULL, 10);
            if (dscp_snd > 63)
                return 1;
            snd_tos = dscp_snd << 2;
            break;

        case 'i':
            interv_msg = strtol(optarg, NULL, 10);
            if (interv_msg > 10000)
                return 1;
            break;

        case 'o':
            otbr = strtol(optarg, NULL, 10);
            authmode = authmode | kModeReflectOctets;
            break;

        case 'c':
            /* TCP control port of the server — must match the server's -c option */
            *tcp_ctrl_port = atoi(optarg);
            if (*tcp_ctrl_port < 1 || *tcp_ctrl_port > 65535) {
                fprintf(stderr,
                        "Invalid TCP control port %d, using default %d\n",
                        *tcp_ctrl_port, SERVER_PORT);
                *tcp_ctrl_port = SERVER_PORT;
            }
            break;

        case '6':
            socket_family = AF_INET6;
            break;

        case 'h':
        default:
            return 1;
        }
    }

    /* -s is mandatory */
    if (server_host == NULL) {
        fprintf(stderr, "Missing mandatory option: -s server\n");
        return 1;
    }

    /* Resolve server hostname */
    *server = (socket_family == AF_INET6) ?
              gethostbyname2(server_host, AF_INET6) :
              gethostbyname(server_host);

    return 0;
}

/* This function sends StopSessions to stop all active Test sessions */
static int send_stop_session(int socket, int accept, int sessions)
{
    StopSessions stop;
    memset(&stop, 0, sizeof(stop));
    stop.Type = kStopSessions;
    stop.Accept = accept;
    stop.SessionsNo = htonl(sessions);
    return send(socket, &stop, sizeof(stop), 0);
}

static int send_start_sessions(int socket)
{
    StartSessions start;
    memset(&start, 0, sizeof(start));
    start.Type = kStartSessions;
    return send(socket, &start, sizeof(start), 0);
}

/* The function will return a significant message for a given code */
static char *get_accept_str(int code)
{
    switch (code) {
    case kOK:
        return "OK.";
    case kFailure:
        return "Failure, reason unspecified.";
    case kInternalError:
        return "Internal error.";
    case kAspectNotSupported:
        return "Some aspect of the request is not supported.";
    case kPermanentResourceLimitation:
        return
            "Cannot perform the request due to permanent resource limitations.";
    case kTemporaryResourceLimitation:
        return
            "Cannot perform the request due to temporary resource limitations.";
    default:
        return "Undefined failure";
    }
}

int main(int argc, char *argv[])
{
    char *progname = argv[0];
    srand(time(NULL));
    if (strrchr(progname, '/') != NULL)
        progname = strrchr(progname, '/') + 1;

    /* Safety check: do not run as root */
    if (getuid() == 0) {
        fprintf(stderr, "%s should not be run as root\n", progname);
        exit(EXIT_FAILURE);
    }

    struct hostent *server = NULL;

    /* Default TCP control port — may be overridden by -c to match
     * the server's -c option */
    int tcp_ctrl_port = SERVER_PORT;

    if (parse_options(&server, argc, argv, &tcp_ctrl_port)) {
        usage(progname);
        exit(EXIT_FAILURE);
    }
    if (server == NULL) {
        fprintf(stderr, "Error: could not resolve server hostname\n");
        exit(EXIT_FAILURE);
    }

    /* Create the TCP control socket */
    int servfd = socket(socket_family, SOCK_STREAM, 0);
    if (servfd < 0) {
        perror("Error opening TCP control socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in  serv_addr;
    struct sockaddr_in6 serv_addr6;
    char str_serv[INET6_ADDRSTRLEN];

    if (socket_family == AF_INET6) {
        memset(&serv_addr6, 0, sizeof(serv_addr6));
        serv_addr6.sin6_family = AF_INET6;
        memcpy(&serv_addr6.sin6_addr, server->h_addr, server->h_length);
        /* FIX: use the configurable tcp_ctrl_port instead of hardcoded SERVER_PORT */
        serv_addr6.sin6_port = htons(tcp_ctrl_port);

        inet_ntop(AF_INET6, &(serv_addr6.sin6_addr),
                  str_serv, sizeof(str_serv));
        printf("Connecting to server %s TCP port %d...\n",
               str_serv, tcp_ctrl_port);

        if (connect(servfd, (struct sockaddr *)&serv_addr6,
                    sizeof(serv_addr6)) < 0) {
            perror("Error connecting");
            close(servfd);
            exit(EXIT_FAILURE);
        }
    } else {
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        /* FIX: use the configurable tcp_ctrl_port */
        serv_addr.sin_port = htons(tcp_ctrl_port);

        inet_ntop(AF_INET, &(serv_addr.sin_addr),
                  str_serv, sizeof(str_serv));
        printf("Connecting to server %s TCP port %d...\n",
               str_serv, tcp_ctrl_port);

        if (connect(servfd, (struct sockaddr *)&serv_addr,
                    sizeof(serv_addr)) < 0) {
            perror("Error connecting");
            close(servfd);
            exit(EXIT_FAILURE);
        }
    }
    /* TWAMP-Control change of messages after TCP connection is established */

    /* Receive Server Greeting and check Modes */
    ServerGreeting greet;
    memset(&greet, 0, sizeof(greet));
    int rv = recv(servfd, &greet, sizeof(greet), 0);
    if (rv <= 0) {
        close(servfd);
        perror("Error receiving Server Greeting");
        exit(EXIT_FAILURE);
    }

    printf("Received ServerGreeting message with mode %d\n",
           ntohl(greet.Modes));

    /* Abort with Mode 0 */
    if ((ntohl(greet.Modes) & 0x000F) == 0) {
        close(servfd);
        fprintf(stderr, "The server does not support any usable Mode\n");
        exit(EXIT_FAILURE);
    }

    /* Compute SetUpResponse */

    SetUpResponse resp;
    memset(&resp, 0, sizeof(resp));
    workmode = ntohl(greet.Modes) & authmode;
    resp.Mode = htonl(workmode);
    printf("Sending SetUpResponse with mode %d\n", workmode);
    rv = send(servfd, &resp, sizeof(resp), 0);
    if (rv <= 0) {
        close(servfd);
        perror("Error sending Greeting Response");
        exit(EXIT_FAILURE);
    }

    /* Set Timeout to exit in case server does not respond to mode 0 */
    if (workmode == 0) {
        //close(servfd);
        fprintf(stderr,
                "The client and server do not support any usable Mode.\n");
        //exit(EXIT_FAILURE);
        int result;

        /* Set Timeout */
        struct timeval timeout = { 5, 0 };  //set timeout for 5 seconds

        /* Set receive UDP message timeout value */
#ifdef SO_RCVTIMEO
        result = setsockopt(servfd, SOL_SOCKET, SO_RCVTIMEO,
                            (char *)&timeout, sizeof(struct timeval));
        if (result != 0) {
            fprintf(stderr,
                    "[PROBLEM] Cannot set the timeout value for reception.\n");
        }
#else
        fprintf(stderr,
                "No way to set the timeout value for incoming packets on that platform.\n");
#endif
    }

    /* Receive ServerStart message */
    ServerStart start;
    memset(&start, 0, sizeof(start));
    rv = recv(servfd, &start, sizeof(start), 0);
    if (rv <= 0) {
        close(servfd);
        perror("Error Receiving Server Start");
        exit(EXIT_FAILURE);
    }
    /* If Server did not accept our request */
    if (start.Accept != kOK) {
        close(servfd);
        fprintf(stderr, "Request failed: %s\n", get_accept_str(start.Accept));
        exit(EXIT_FAILURE);
    }

    /* Retrieve the local IP address assigned to the control connection */
    char str_host[INET6_ADDRSTRLEN];
    struct sockaddr_in  host_addr;
    struct sockaddr_in6 host_addr6;

    if (socket_family == AF_INET6) {
        /* FIX: was writing into host_addr (IPv4) and reading from uninitialised
         * host_addr6 — both sides now consistently use host_addr6 */
        socklen_t addr_size = sizeof(host_addr6);
        memset(&host_addr6, 0, sizeof(host_addr6));
        getsockname(servfd, (struct sockaddr *)&host_addr6, &addr_size);
        inet_ntop(AF_INET6, &(host_addr6.sin6_addr),
                  str_host, sizeof(str_host));
    } else {
        socklen_t addr_size = sizeof(host_addr);
        memset(&host_addr, 0, sizeof(host_addr));
        getsockname(servfd, (struct sockaddr *)&host_addr, &addr_size);
        inet_ntop(AF_INET, &(host_addr.sin_addr),
                  str_host, sizeof(str_host));
    }

    printf("Received ServerStart at Client %s\n", str_host);

    /* After the TWAMP-Control connection has been established, the
     * Control-Client will negociate and set up some TWAMP-Test sessions */

    struct twamp_test_info *twamp_test =
        malloc(test_sessions_no * sizeof(struct twamp_test_info));
    if (!twamp_test) {
        fprintf(stderr, "Error on malloc\n");
        close(servfd);
        exit(EXIT_FAILURE);
    }

    uint16_t i;
    /* Set TWAMP-Test sessions */
        for (i = 0; i < test_sessions_no; i++) {

        /* FIX: use socket_family instead of the hardcoded AF_INET so that
         * IPv6 test sessions get a proper IPv6 UDP socket */
        twamp_test[active_sessions].testfd =
            socket(socket_family, SOCK_DGRAM, 0);
        if (twamp_test[active_sessions].testfd < 0) {
            perror("Error opening UDP test socket");
            continue;
        }

        /* FIX: use an explicit bind_ok flag instead of checking check_time < 0
         * (after the last decrement check_time reaches 0, not -1, so the
         * original condition missed the case where every attempt failed) */
        int check_time = CHECK_TIMES;
        int bind_ok    = 0;

        if (socket_family == AF_INET6) {
            struct sockaddr_in6 local_addr6;
            memset(&local_addr6, 0, sizeof(local_addr6));
            local_addr6.sin6_family = AF_INET6;
            local_addr6.sin6_addr   = in6addr_any;

            while (check_time--) {
                twamp_test[active_sessions].testport =
                    port_send + rand() % 1000;
                local_addr6.sin6_port =
                    htons(twamp_test[active_sessions].testport);
                if (!bind(twamp_test[active_sessions].testfd,
                          (struct sockaddr *)&local_addr6,
                          sizeof(local_addr6))) {
                    bind_ok = 1;
                    break;
                }
            }
        } else {
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family      = AF_INET;
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

            while (check_time--) {
                twamp_test[active_sessions].testport =
                    port_send + rand() % 1000;
                local_addr.sin_port =
                    htons(twamp_test[active_sessions].testport);
                if (!bind(twamp_test[active_sessions].testfd,
                          (struct sockaddr *)&local_addr,
                          sizeof(struct sockaddr))) {
                    bind_ok = 1;
                    break;
                }
            }
        }

        if (!bind_ok) {
            fprintf(stderr,
                    "Session %d: could not bind a sender port "
                    "in range [%ld..%ld]: %s\n",
                    i + 1, port_send, port_send + 999,
                    strerror(errno));
            close(twamp_test[active_sessions].testfd);
            continue;
        }

        /* Set socket options for TTL and TOS retrieval via ancillary data */
        set_socket_option(twamp_test[active_sessions].testfd, HDR_TTL);
        set_socket_tos(twamp_test[active_sessions].testfd, snd_tos);

        /* FIX: set a receive timeout on the UDP test socket so that
         * recvmsg() does not block forever if the reflector is unreachable */
        {
            struct timeval udp_timeout = { TIMEOUT, 0 };
            if (setsockopt(twamp_test[active_sessions].testfd,
                           SOL_SOCKET, SO_RCVTIMEO,
                           &udp_timeout, sizeof(udp_timeout)) < 0)
                perror("Warning: could not set UDP receive timeout");
        }

        /* Build and send the RequestTWSession message */
        RequestSession req;
        memset(&req, 0, sizeof(req));
        req.Type = kRequestTWSession;
        /* FIX: reflect the actual IP version in use instead of hardcoding 4 */
        req.IPVN = (socket_family == AF_INET6) ? 6 : 4;

        req.SenderPort   = htons(twamp_test[active_sessions].testport);
        /* port_recv is a hint; the server will select the real port and
         * return it in AcceptSession.Port */
        req.ReceiverPort = htons(port_recv + rand() % 1000);

        if (socket_family == AF_INET) {
            req.SenderAddress   = host_addr.sin_addr.s_addr;
            req.ReceiverAddress = serv_addr.sin_addr.s_addr;
        }
        /* IPv6 addresses would go in the extended address fields if supported */

        if ((workmode & KModeSymmetrical) == KModeSymmetrical) {
            mbz_offset = 27;
            if ((workmode & kModeDSCPECN) == kModeDSCPECN)
                mbz_offset = 28;
        }

        /* PaddingLength as defined in RFC 6038 §4.2 */
        req.PaddingLength = htonl(payload_len - 14 - mbz_offset);

        TWAMPTimestamp timestamp = get_timestamp();
        timestamp.integer =
            htonl(ntohl(timestamp.integer) + STTIME);
        req.StartTime = timestamp;

        req.Timeout.integer    = htonl(TIMEOUT);
        req.Timeout.fractional = htonl(0);
        req.TypePDescriptor    = htonl((snd_tos & 0xFC) << 22);

        if ((workmode & kModeReflectOctets) == kModeReflectOctets) {
            req.OctetsToBeReflected = htons(otbr);
            req.PadLenghtToReflect  = htons(2);
        }

        printf("Session %d: sending RequestTWSession "
               "(sender port %d, receiver hint port %d)...\n",
               i + 1,
               twamp_test[active_sessions].testport,
               ntohs(req.ReceiverPort));

        rv = send(servfd, &req, sizeof(req), 0);
        if (rv <= 0) {
            perror("Error sending RequestTWSession");
            close(twamp_test[active_sessions].testfd);
            free(twamp_test);
            close(servfd);
            exit(EXIT_FAILURE);
        }

        /* Read the server's AcceptSession response */
        AcceptSession acc;
        memset(&acc, 0, sizeof(acc));
        rv = recv(servfd, &acc, sizeof(acc), 0);
        if (rv <= 0) {
            perror("Error receiving AcceptSession");
            close(twamp_test[active_sessions].testfd);
            free(twamp_test);
            close(servfd);
            exit(EXIT_FAILURE);
        }

        if (acc.Accept != kOK) {
            fprintf(stderr, "Session %d rejected by server: %s\n",
                    i + 1, get_accept_str(acc.Accept));
            close(twamp_test[active_sessions].testfd);
            continue;
        }

        /* Use the port chosen by the server, not the hint we sent */
        twamp_test[active_sessions].port = ntohs(acc.Port);
        printf("Session %d accepted: reflector UDP port = %d\n",
               i + 1, twamp_test[active_sessions].port);

        /* Log the SID returned by the server */
        {
            uint32_t       sid_addr;
            TWAMPTimestamp sid_time;
            uint32_t       sid_rand;
            memcpy(&sid_addr, &acc.SID,      4);
            memcpy(&sid_time, &acc.SID[4],   8);
            memcpy(&sid_rand, &acc.SID[12],  4);
            fprintf(stderr, "SID: 0x%04X.%04X.%04X.%04X\n",
                    ntohl(sid_addr),
                    ntohl(sid_time.integer),
                    ntohl(sid_time.fractional),
                    ntohl(sid_rand));
        }

        twamp_test[active_sessions].serveroct = acc.ServerOctets;

        fprintf(stderr,
                "Session %d: sender %s:%d → reflector %s:%d  mode=%d\n",
                active_sessions,
                str_host, twamp_test[active_sessions].testport,
                str_serv, twamp_test[active_sessions].port,
                workmode);

        if ((workmode & kModeReflectOctets) == kModeReflectOctets)
            fprintf(stderr,
                    "  OTBR=%u, ReflectedOctets=%d, ServerOctets=%d\n",
                    otbr,
                    ntohs(acc.ReflectedOctets),
                    ntohs(twamp_test[active_sessions].serveroct));

        active_sessions++;
    }

    fprintf(stderr, "Nb of Packets \t%u, Packet length \t%d, DSCP \t%d,"
            " TOS \t%d\n", test_sessions_msg, payload_len,
            (snd_tos & 0xFC) >> 2, snd_tos);
    if (active_sessions) {
        printf("Sending Start-Sessions for all active Sender ports...\n");

        /* If there are any accepted Test-Sessions then send
         * the StartSessions message */
        rv = send_start_sessions(servfd);
        if (rv <= 0) {
            perror("Error sending StartSessions");
            /* Close all TWAMP-Test sockets */
            for (i = 0; i < active_sessions; i++)
                close(twamp_test[i].testfd);
            free(twamp_test);
            close(servfd);
            exit(EXIT_FAILURE);
        }
        sleep(WAIT);
    }

    /* For each accepted TWAMP-Test session send test_sessions_msg
     * TWAMP-Test packets */
        for (i = 0; i < active_sessions; i++) {
        uint32_t j;

        fprintf(stderr,
                "\tTime\t, Snd#\t, Rcv#\t, SndPt\t, RcvPt\t,  Sync\t,"
                " FW TTL, SW TTL, SndTOS, FW_TOS, SW_TOS,"
                " NwRTD\t, IntD\t, FWD\t, SWD  [ms]\n");

        uint32_t lost_msg    = 0;
        uint32_t fw_lost_msg = 0;
        uint32_t sw_lost_msg = 0;
        uint32_t rt_msg      = 0;
        uint32_t rcv_sn      = 0;
        uint32_t snd_sn      = 0;
        uint32_t index       = 0;
        uint64_t rtd         = LOSTTIME * 1000000;

        for (j = 0; j < test_sessions_msg; j++) {

            /* Build and send the TWAMP-Test packet */
            SenderUPacket pack;
            memset(&pack, 0, sizeof(pack));
            index              = 0 * test_sessions_msg + j;
            pack.seq_number    = htonl(index);
            pack.time          = get_timestamp();
            pack.error_estimate = htons(0x8001); /* Sync=1, Multiplier=1 */

            memcpy(&pack.padding[mbz_offset],
                   &twamp_test[i].serveroct, 2);

            if (socket_family == AF_INET6) {
                serv_addr6.sin6_port = htons(twamp_test[i].port);
                rv = sendto(twamp_test[i].testfd, &pack, payload_len, 0,
                            (struct sockaddr *)&serv_addr6,
                            sizeof(serv_addr6));
            } else {
                serv_addr.sin_port = htons(twamp_test[i].port);
                rv = sendto(twamp_test[i].testfd, &pack, payload_len, 0,
                            (struct sockaddr *)&serv_addr,
                            sizeof(serv_addr));
            }

            if (rv <= 0) {
                perror("Error sending TWAMP-Test packet");
                continue;
            }

            /* Prepare ancillary data structures for recvmsg.
             * FIX: allocate iov separately so every pointer is tracked
             * and freed on every exit path (including continue) */
            ReflectorUPacket pack_reflect;
            memset(&pack_reflect, 0, sizeof(pack_reflect));

            struct msghdr *message = malloc(sizeof(struct msghdr));
            struct iovec  *iov     = malloc(sizeof(struct iovec));
            char          *ctrl_buf = malloc(TST_PKT_SIZE);

            if (!message || !iov || !ctrl_buf) {
                fprintf(stderr,
                        "malloc failed for recvmsg buffers\n");
                free(message);
                free(iov);
                free(ctrl_buf);
                lost_msg++;
                continue;
            }

            memset(message, 0, sizeof(*message));
            iov->iov_base        = &pack_reflect;
            iov->iov_len         = TST_PKT_SIZE;
            message->msg_iov     = iov;
            message->msg_iovlen  = 1;
#ifndef NO_MESSAGE_CONTROL
            message->msg_control    = ctrl_buf;
            message->msg_controllen = TST_PKT_SIZE;
#endif

            int recv_rv = recvmsg(twamp_test[i].testfd, message, 0);

            /* Capture receive timestamp as early as possible */
            TWAMPTimestamp rcv_resp_time = get_timestamp();

            if (recv_rv <= 0) {
                /* Packet lost or timeout (SO_RCVTIMEO) */
                uint64_t t_sender_usec = get_usec(&pack.time);
                fprintf(stderr,
                        "%.0f\t, %3d\t, %3c\t, %d\t, %d\t,"
                        " %3c\t, %3c\t, %3c\t, %3c\t, %3c\t,"
                        " %3c\t, %3c\t, %3c\t, %3c\t, %3c\n",
                        (double)t_sender_usec * 1e-3, (int)index, '-',
                        twamp_test[i].testport, twamp_test[i].port,
                        '-', '-', '-', '-', '-', '-', '-', '-', '-', '-');

                rtd = LOSTTIME * 1000000;
                lost_msg++;

                if (((j + 1) % 10) == 0 && lost_msg)
                    printf("RT Lost packets: %u/%u, RT Loss Ratio: %3.2f%%\n",
                           lost_msg, (j + 1),
                           (float)100 * lost_msg / (j + 1));

                /* FIX: always free before continue */
                free(iov);
                free(message);
                free(ctrl_buf);
                continue;
            }

            /* Extract TTL and TOS from IP header via ancillary data */
            uint8_t sw_ttl = 0;
            uint8_t sw_tos = 0;
            struct cmsghdr *c_msg;

#ifndef NO_MESSAGE_CONTROL
            for (c_msg = CMSG_FIRSTHDR(message); c_msg;
                 c_msg = CMSG_NXTHDR(message, c_msg)) {
                if ((c_msg->cmsg_level == IPPROTO_IP &&
                     c_msg->cmsg_type  == IP_TTL)
                 || (c_msg->cmsg_level == IPPROTO_IPV6 &&
                     c_msg->cmsg_type  == IPV6_HOPLIMIT)) {
                    sw_ttl = *(int *)CMSG_DATA(c_msg);
                } else if (c_msg->cmsg_level == IPPROTO_IP &&
                           c_msg->cmsg_type  == IP_TOS) {
                    sw_tos = *(int *)CMSG_DATA(c_msg);
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

            /* Update one-way loss counters */
            if (rt_msg == 0) {
                rt_msg = 1;
            } else {
                fw_lost_msg += index - snd_sn -
                               ntohl(pack_reflect.seq_number) + rcv_sn;
                sw_lost_msg += ntohl(pack_reflect.seq_number) - rcv_sn - 1;
                rt_msg      += index - snd_sn;
            }

            /* Print per-packet latency metrics */
            rtd = print_metrics(twamp_test[i].testport,
                                twamp_test[i].port,
                                snd_tos, sw_ttl, sw_tos,
                                &rcv_resp_time, &pack_reflect, workmode);

            /* Update sequence number tracking */
            snd_sn = index;
            rcv_sn = ntohl(pack_reflect.seq_number);

            /* Print loss summary every 10 packets (not on the last burst) */
            if ((((j + 1) % 10) == 0) && ((j + 1) < test_sessions_msg)) {
                printf("RT Lost packets: %u/%u, RT Loss Ratio: %3.2f%%\n",
                       lost_msg, (j + 1),
                       (float)100 * lost_msg / (j + 1));
                if (rt_msg && lost_msg) {
                    printf("FW Lost packets: %u/%u, FW Loss Ratio: %3.2f%%\n",
                           fw_lost_msg, rt_msg,
                           (float)100 * fw_lost_msg / rt_msg);
                    if (rt_msg > fw_lost_msg)
                        printf("SW Lost packets: %u/%u, SW Loss Ratio: %3.2f%%\n",
                               sw_lost_msg, rt_msg - fw_lost_msg,
                               (float)100 * sw_lost_msg /
                               (rt_msg - fw_lost_msg));
                }
            }

            /* FIX: free heap buffers at the end of every successful iteration */
            free(iov);
            free(message);
            free(ctrl_buf);

            /* Sleep for the remainder of the interval if needed */
            if (rtd < interv_msg * 1000)
                usleep((useconds_t)(interv_msg * 1000 - rtd));
        }

        /* Print final loss results for this session */
        printf("--- Session %d final statistics ---\n", i);
        printf("RT Lost packets: %u/%u, RT Loss Ratio: %3.2f%%\n",
               lost_msg, test_sessions_msg,
               (float)100 * lost_msg / test_sessions_msg);
        if (rt_msg && lost_msg) {
            printf("FW Lost packets: %u/%u, FW Loss Ratio: %3.2f%%\n",
                   fw_lost_msg, rt_msg,
                   (float)100 * fw_lost_msg / rt_msg);
            if (rt_msg > fw_lost_msg)
                printf("SW Lost packets: %u/%u, SW Loss Ratio: %3.2f%%\n",
                       sw_lost_msg, rt_msg - fw_lost_msg,
                       (float)100 * sw_lost_msg / (rt_msg - fw_lost_msg));
        }
    }

    /* After all TWAMP-Test packets were sent, send a StopSessions
     * packet and finish */
    if (active_sessions) {

        printf("Sending Stop-Sessions for all active ports...\n");
        rv = send_stop_session(servfd, kOK, active_sessions);
        if (rv <= 0) {
            perror("Error sending stop session");
            /* Close all TWAMP-Test sockets */
            for (i = 0; i < active_sessions; i++)
                close(twamp_test[i].testfd);
            free(twamp_test);
            close(servfd);
            exit(EXIT_FAILURE);
        }
    }
    /* Close all TWAMP-Test sockets */
    for (i = 0; i < active_sessions; i++)
        close(twamp_test[i].testfd);
    free(twamp_test);
    close(servfd);
    return 0;
}
