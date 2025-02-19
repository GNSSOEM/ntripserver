/*
 * $Id: ntripserver.c 9812 2022-08-23 14:30:54Z stoecker $
 *
 * Copyright (c) 2003...2019
 * German Federal Agency for Cartography and Geodesy (BKG)
 * Dirk StÃ¶cker (Alberding GmbH)
 *
 * Developed for Networked Transport of RTCM via Internet Protocol (NTRIP)
 * for streaming GNSS data over the Internet.
 *
 * Designed by Informatik Centrum Dortmund http://www.icd.de
 *
 * The BKG disclaims any liability nor responsibility to any person or
 * entity with respect to any loss or damage caused, or alleged to be
 * caused, directly or indirectly by the use and application of the NTRIP
 * technology.
 *
 * For latest information and updates, access:
 * https://igs.bkg.bund.de/ntrip/index
 *
 * BKG, Frankfurt, Germany, August 2019
 * E-mail: euref-ip@bkg.bund.de
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* SVN revision and version */
static char revisionstr[] = "$Revision: 9812 $";
static char datestr[] = "$Date: 2022-08-23 14:30:54 +0000 (Tue, 23 Aug 2022) $";

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#ifdef WINDOWSVERSION
  #include <winsock2.h>
  #include <io.h>
  #include <sys/stat.h>
  #include <windows.h>
  typedef SOCKET sockettype;
  typedef u_long in_addr_t;
  typedef size_t socklen_t;
  typedef u_short uint16_t;
#else
typedef int sockettype;
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/termios.h>
#define closesocket(sock) close(sock)
#define INVALID_HANDLE_VALUE -1
#define INVALID_SOCKET -1
#endif

#ifndef COMPILEDATE
#define COMPILEDATE " built " __DATE__
#endif

#define ALARMTIME (2*60)

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0 /* prevent compiler errors */
#endif
#ifndef O_EXLOCK
#define O_EXLOCK 0 /* prevent compiler errors */
#endif

enum MODE {
  SERIAL = 1,
  TCPSOCKET = 2,
  INFILE = 3,
  SISNET = 4,
  UDPSOCKET = 5,
  NTRIP1_IN = 6,
  NTRIP2_HTTP_IN = 7, // HTTP only
  LAST
};

enum OUTMODE {
  HTTP = 1,
  RTSP = 2,
  NTRIP1 = 3,
  UDP = 4,
  TCPIP = 5,
  END
};

enum EXITMODE {
  SUCCESS = 0,
  CMD_KEY_ERROR = 1,
  CMD_VALUE_ERROR = 2,
  IO_ERROR = 3,
  NET_ERROR = 4
};

#define AGENTSTRING     "NTRIP NtripServerPOSIX"
#define BUFSZ           10240
#define SZ              64

/* default socket source */
#define SERV_HOST_ADDR  "localhost"
#define SERV_TCP_PORT   2101

/* default destination */
#define NTRIP_CASTER    "euref-ip.net"
#define NTRIP_PORT      2101

#define SISNET_SERVER   "131.176.49.142"
#define SISNET_PORT     7777

#define RTP_VERSION     2
#define TIME_RESOLUTION 125

static int ttybaud = 19200;
#ifndef WINDOWSVERSION
static const char *ttyport = "/dev/gps";
#else
  static const char *ttyport = "COM1";
#endif
static const char *filepath = "/dev/stdin";
static enum MODE inputmode = INFILE;
static int sisnet = 31;
static int gps_file = -1;
static sockettype gps_socket = INVALID_SOCKET;
static sockettype socket_tcp = INVALID_SOCKET;
static sockettype local_socket_tcp = INVALID_SOCKET;
static sockettype socket_udp = INVALID_SOCKET;
#ifndef WINDOWSVERSION
static int gps_serial = INVALID_HANDLE_VALUE;
static int sigpipe_received = 0;
#else
  HANDLE gps_serial = INVALID_HANDLE_VALUE;
#endif
static int in_reconnect_pause = 0;
static int sigalarm_received = 0;
static int sigint_received = 0;
static int reconnect_sec = 1;
static const char *casterouthost = NTRIP_CASTER;
static unsigned int casteroutport = NTRIP_PORT;
static int currentoutputmode = NTRIP1;
static char rtsp_extension[SZ] = "";
static const char *mountpoint = NULL;
const char *nrip1Mountpoint = NULL;
static int udp_cseq = 1;
static int udp_tim, udp_seq, udp_init;
const char *flagpath = "";
const char *outhost = 0;
unsigned int outport = 0;
int output_v2http_chunken = 1;
int was_1st_ntrip_connect = 0;
int reconnect_sec_max = 0;

/* Forward references */
static void send_receive_loop(sockettype sock, int outmode,
    struct sockaddr *pcasterRTP, socklen_t length, unsigned int rtpssrc,
    int chunkymode);
static void usage(int, char*);
static int encode(char *buf, int size, const char *user, const char *pwd);
static int send_to_caster(char *input, sockettype socket, int input_size);
static void close_session(const char *caster_addr, const char *mountpoint,
    int session, char *rtsp_ext, int fallback);
static int reconnect(int rec_sec, int rec_sec_max);
static void handle_sigint(int sig);
static void setup_signal_handler(int sig, void (*handler)(int));
#ifndef WINDOWSVERSION
static int openserial(const char *tty, int blocksz, int baud);
static void handle_sigpipe(int sig);
static void handle_alarm(int sig);
#else
  static HANDLE openserial(const char * tty, int baud);
#endif

#define FLAG_MSG_SIZE 1024
#define NTRIP_MINRSP 14
#define MAX_NTRIP_CONNECT_TIME   5000  /* 5 sec    = 5 000 ms */
#define NTRIPv1_RSP_OK_SVR    "OK\r\n"               /* ntrip v1 response: server OK */
#define NTRIPv2_RSP_OK_SVR    "HTTP/1.1 200 OK"      /* ntrip v2 response: server OK */
#define CODE_RSP_OK_SVR       " 200 "                /* ntrip v2 response: server OK */
#define NTRIPv1_RSP_ERROR     "Bad Request"          /* ntrip v1 response: error */
#define NTRIPv2_RSP_ERROR     "ERROR"                /* ntrip v2 response: error */
#define CODE_RSP_ERROR        " 500 "                /* ntrip v2 response: error */
#define NTRIPv1_RSP_NOT_FOUND "Mount Point Invalid"  /* ntrip v1 response: not found */
#define NTRIPv2_RSP_NOT_FOUND "Not Found"            /* ntrip v2 response: not found */
#define CODE_RSP_NOT_FOUND    " 404 "                /* ntrip v2 response: not found */
#define NTRIPv1_RSP_UNAUTH    "Bad Password"         /* ntrip v1 response: unauthorized */
#define NTRIPv2_RSP_UNAUTH    "Unauthorized"         /* ntrip v2 response: unauthorized */
#define CODE_RSP_UNAUTH       " 401 "                /* ntrip v2 response: unauthorized */
#define NTRIPv1_RSP_USED1     "Mount Point Taken"    /* ntrip v1 response: already used */
#define NTRIPv2_RSP_USED      "Conflict"             /* ntrip v2 response: already used */
#define CODE_RSP_USED         " 409 "                  /* ntrip v2 response: already used */
#define NTRIPv1_RSP_USED2     "Mount Point Taken or Invalid"  /* ntrip v1 response: not found */
#define NTRIPv2_RSP_NOTIMPL   "Implemented"          /* ntrip v2 response: not implemented */
#define CODE_RSP_NOTIMPL      " 501 "                /* ntrip v2 response: not implemented */
#define NTRIPv2_RSP_UNAVAIL   "Unavailable"          /* ntrip v2 response: unavailable */
#define CODE_RSP_UNAVAIL      " 503 "          /* ntrip v2 response: unavailable */
#define NTRIPv2_RSP_VERSION   "Ntrip-Version: Ntrip/2" /* ntrip v2 response: version */
#define NTRIP_MAXRSP          2048                   /* max size of ntrip response */
#define MIN_PAUSE_AT_EMPTY    15                     /* min pause for EMPTY response */
#define MIN_PAUSE_AT_UNAUTH   30                     /* min pause for UNAUTHORIZED response */

static uint32_t tickget(void);
static int str_as_printable(const char *szSendBuffer, int nBufferBytes, char *msgbuf, size_t msgbufSize);
static void set_reconnect_time_at_hard_error(void);
static int recv_from_caster(char *szSendBuffer, size_t bufferSize, char *msgbuf, size_t msgbufSize, const char *protocolName);
static int errsock(void);
static char *errorstring(int err);
static void flag_create(const char *msg);
static void flag_erase(void);
static void flag_socket_error(const char *msg);
static void flag_io_error(const char *msg);
static void flag_logical_error(const char *msg);
static void flag_str_error(const char *msg, const char *arg);
static void flag_int_error(const char *msg, int value);

/*
 * main
 *
 * Main entry point for the program.  Processes command-line arguments and
 * prepares for action.
 *
 * Parameters:
 *     argc : integer        : Number of command-line arguments.
 *     argv : array of char  : Command-line arguments as an array of
 *                             zero-terminated pointers to strings.
 *
 * Return Value:
 *     The function does not return a value (although its return type is int).
 *
 * Remarks:
 *
 */
int main(int argc, char **argv) {
  int c;
  int size = 2048; /* for setting send buffer size */
  struct sockaddr_in caster;
  const char *proxyhost = "";
  unsigned int proxyport = 0;
  char msgbuf[FLAG_MSG_SIZE];
  int msglen;

  /*** INPUT ***/
  const char *casterinhost = 0;
  unsigned int casterinport = 0;
  const char *inhost = 0;
  unsigned int inport = 0;
  int chunkymode = 0;
  char get_extension[SZ] = "";

  struct hostent *he;

  const char *sisnetpassword = "";
  const char *sisnetuser = "";

  const char *stream_name = 0;
  const char *stream_user = 0;
  const char *stream_password = 0;

  const char *recvrid = 0;
  const char *recvrpwd = 0;

  const char *initfile = NULL;

  int bindmode = 0;

  /*** OUTPUT ***/
  char post_extension[SZ] = "";

  const char *ntrip_str = "";

  const char *user = "";
  const char *password = "";

  int outputmode = NTRIP1;
  int useNTRIP1 = 0; // 0 - нет, 1 - да

  struct sockaddr_in casterRTP;
  struct sockaddr_in local;
  int client_port = 0;
  int server_port = 0;
  unsigned int session = 0;
  socklen_t len = 0;
  int i = 0;

  char szSendBuffer[BUFSZ];
  char authorization[SZ];
  int nBufferBytes = 0;
  char *dlim = " \r\n=";
  char *token;
  char *tok_buf[BUFSZ];

  setbuf(stdout, 0);
  setbuf(stdin, 0);
  setbuf(stderr, 0);

  {
    char *a;
    int i = 2;
    strcpy(revisionstr, "1.");
    for (a = revisionstr + 11; *a && *a != ' '; ++a)
      revisionstr[i++] = *a;
    revisionstr[i] = 0;
    i = 0;
    for (a = datestr + 7; *a && *a != ' '; ++a)
      datestr[i++] = *a;
    datestr[i] = 0;
  }

  /* setup signal handler for CTRL+C */
  setup_signal_handler(SIGINT, handle_sigint);
#ifndef WINDOWSVERSION
  /* setup signal handler for boken pipe */
  setup_signal_handler(SIGPIPE, handle_sigpipe);
  /* setup signal handler for timeout */
  setup_signal_handler(SIGALRM, handle_alarm);
  alarm(ALARMTIME);
#else
  /* winsock initialization */
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(1,1), &wsaData))
  {
    flag_socket_error("Could not init network access");
    return 20;
  }
#endif

  /* get and check program arguments */
  if (argc <= 1) {
    usage(2, argv[0]);
    exit(CMD_KEY_ERROR);
  }

  flag_create("Starting...");

  while ((c = getopt(argc, argv,
      "M:i:h:b:p:s:a:m:c:H:P:f:x:y:l:u:V:D:U:W:O:E:F:R:N:n:BL:")) != EOF) {
    switch (c) {
      case 'M': /*** InputMode ***/
        if (!strcmp(optarg, "serial"))
          inputmode = SERIAL;
        else if (!strcmp(optarg, "tcpsocket"))
          inputmode = TCPSOCKET;
        else if (!strcmp(optarg, "file"))
          inputmode = INFILE;
        else if (!strcmp(optarg, "sisnet"))
          inputmode = SISNET;
        else if (!strcmp(optarg, "udpsocket"))
          inputmode = UDPSOCKET;
        else if (!strcmp(optarg, "ntrip1"))
          inputmode = NTRIP1_IN;
        else if (!strcmp(optarg, "ntrip2http"))
          inputmode = NTRIP2_HTTP_IN;
        else
          inputmode = atoi(optarg);
        if ((inputmode == 0) || (inputmode >= LAST)) {
          flag_str_error("ERROR: can't convert <%s> to a valid InputMode", optarg);
          exit(CMD_VALUE_ERROR);
        }
        break;
      case 'i': /* serial input device */
        ttyport = optarg;
        break;
      case 'B': /* bind to incoming UDP stream */
        bindmode = 1;
        break;
      case 'V': /* Sisnet data server version number */
        if (!strcmp("3.0", optarg))
          sisnet = 30;
        else if (!strcmp("3.1", optarg))
          sisnet = 31;
        else if (!strcmp("2.1", optarg))
          sisnet = 21;
        else {
          flag_str_error("ERROR: unknown SISNeT version <%s>", optarg);
          exit(CMD_VALUE_ERROR);
        }
        break;
      case 'b': /* serial input baud rate */
        ttybaud = atoi(optarg);
        if (ttybaud <= 1) {
          flag_str_error("ERROR: can't convert <%s> to valid serial baud rate", optarg);
          exit(CMD_VALUE_ERROR);
        }
        break;
      case 'a': /* Destination caster address */
        casterouthost = optarg;
        break;
      case 'p': /* Destination caster port */
        casteroutport = atoi(optarg);
        if (casteroutport <= 1 || casteroutport > 65535) {
          flag_str_error("ERROR: can't convert <%s> to a valid HTTP server port", optarg);
          exit(CMD_VALUE_ERROR);
        }
        break;
      case 'm': /* Destination caster mountpoint for stream upload */
        mountpoint = optarg;
        break;
      case 's': /* File name for input data simulation from file */
        filepath = optarg;
        break;
      case 'f': /* name of an initialization file */
        initfile = optarg;
        break;
      case 'x': /* user ID to access incoming stream */
        recvrid = optarg;
        break;
      case 'y': /* password to access incoming stream */
        recvrpwd = optarg;
        break;
      case 'u': /* Sisnet data server user ID */
        sisnetuser = optarg;
        break;
      case 'l': /* Sisnet data server password */
        sisnetpassword = optarg;
        break;
      case 'c': /* DestinationCaster password for stream upload to mountpoint */
        password = optarg;
        break;
      case 'H': /* Input host address*/
        casterinhost = optarg;
        break;
      case 'P': /* Input port */
        casterinport = atoi(optarg);
        if (casterinport <= 1 || casterinport > 65535) {
          flag_str_error("ERROR: can't convert <%s> to a valid port number", optarg);
          exit(CMD_VALUE_ERROR);
        }
        break;
      case 'D': /* Source caster mountpoint for stream input */
        stream_name = optarg;
        break;
      case 'U': /* Source caster user ID for input stream access */
        stream_user = optarg;
        break;
      case 'W': /* Source caster password for input stream access */
        stream_password = optarg;
        break;
      case 'E': /* Proxy Server */
        proxyhost = optarg;
        break;
      case 'F': /* Proxy port */
        proxyport = atoi(optarg);
        break;
      case 'R': /* maximum delay between reconnect attempts in seconds */
        reconnect_sec_max = atoi(optarg);
        break;
      case 'O': /* OutputMode */
        outputmode = 0;
        if (!strcmp(optarg, "n") || !strcmp(optarg, "ntrip1"))
          outputmode = NTRIP1;
        else if (!strcmp(optarg, "h") || !strcmp(optarg, "http"))
          outputmode = HTTP;
        else if (!strcmp(optarg, "r") || !strcmp(optarg, "rtsp"))
          outputmode = RTSP;
        else if (!strcmp(optarg, "u") || !strcmp(optarg, "udp"))
          outputmode = UDP;
        else if (!strcmp(optarg, "t") || !strcmp(optarg, "tcpip"))
          outputmode = TCPIP;
        else
          outputmode = atoi(optarg);
        if ((outputmode == 0) || (outputmode >= END)) {
          flag_str_error("ERROR: can't convert <%s> to a valid OutputMode", optarg);
          exit(CMD_VALUE_ERROR);
        }
        break;
      case 'n': /* Destination caster user ID for stream upload to mountpoint */
        user = optarg;
        break;
      case 'N': /* Ntrip-STR, optional for Ntrip Version 2.0 */
        ntrip_str = optarg;
        break;
      case 'L': /* *No connect" flag file */
        flagpath = optarg;
        break;
      case 'h': /* print help screen */
      case '?':
        usage(0, argv[0]);
        break;
      default:
        usage(CMD_KEY_ERROR, argv[0]);
        break;
    }
  }

  argc -= optind;
  argv += optind;

  /*** argument analysis ***/
  if (argc > 0) {
    msglen = snprintf(msgbuf, sizeof(msgbuf), "ERROR: Extra args on command line: ");
    for (; argc > 0; argc--) {
      msglen += snprintf(msgbuf+msglen, sizeof(msgbuf)-msglen, " %s", *argv++);
    }
    flag_logical_error(msgbuf);
    exit(CMD_KEY_ERROR);
  }

  if ((reconnect_sec_max > 0) && (reconnect_sec_max < 256)) {
    flag_int_error(
        "WARNING: maximum delay between reconnect attempts changed from %d to 256 seconds\n",
        reconnect_sec_max);
    reconnect_sec_max = 256;
  }

  if (!mountpoint && outputmode != TCPIP) {
    flag_logical_error("ERROR: Missing mountpoint argument for stream upload");
    exit(CMD_KEY_ERROR);
  }
  if (outputmode == TCPIP) {
    mountpoint = NULL;
  }

  if (!password[0]) {
    if (outputmode != TCPIP)
      flag_logical_error("WARNING: Missing password argument for stream upload - are you really sure?");
  } else {
    nBufferBytes += encode(authorization, sizeof(authorization), user,
        password);
    if (nBufferBytes > (int) sizeof(authorization)) {
      snprintf(msgbuf, sizeof(msgbuf), "ERROR: user ID and/or password too long: %d (%d) user ID: %s password: <%s>",
               nBufferBytes, (int) sizeof(authorization), user, password);
      flag_logical_error(msgbuf);
      exit(CMD_VALUE_ERROR);
    }
  }

  if (stream_name && stream_user && !stream_password) {
    flag_logical_error("WARNING: Missing password argument for stream download - are you really sure?");
  }

  /*** proxy server handling ***/
  if (*proxyhost) {
    inhost = proxyhost;
    inport = proxyport;
    i = snprintf(szSendBuffer, sizeof(szSendBuffer), "http://%s:%d", casterinhost, casterinport);
    if ((i > SZ) || (i < 0)) {
      snprintf(msgbuf, sizeof(msgbuf), "ERROR: Destination caster name/port to long - length = %d (max: %d)\n", i, SZ);
      flag_logical_error(msgbuf);
      exit(CMD_VALUE_ERROR);
    } else {
      strncpy(get_extension, szSendBuffer, (size_t) i);
      get_extension[SZ-1] = 0;
      strcpy(szSendBuffer, "");
      i = 0;
    }
    if (strstr(casterouthost, "127.0.0.1") ||
        strstr(casterouthost, "localhost")) {
      outhost = casterouthost;
      outport = casteroutport;
    }
    else {
      outhost = proxyhost;
      outport = proxyport;
      i = snprintf(szSendBuffer, sizeof(szSendBuffer), "http://%s:%d", casterouthost, casteroutport);
      if ((i > SZ) || (i < 0)) {
        snprintf(msgbuf, sizeof(msgbuf), "ERROR: Destination caster name/port to long - length = %d (max: %d)\n", i, SZ);
        flag_logical_error(msgbuf);
        exit(CMD_VALUE_ERROR);
      } else {
        strncpy(post_extension, szSendBuffer, (size_t) i);
        post_extension[SZ-1] = 0;
        strcpy(szSendBuffer, "");
        i = snprintf(szSendBuffer, sizeof(szSendBuffer), ":%d", casteroutport);
        strncpy(rtsp_extension, szSendBuffer, SZ);
        rtsp_extension[SZ-1] = 0;
        strcpy(szSendBuffer, ""); i = 0;
      }
    }

  } else {
    outhost = casterouthost;
    outport = casteroutport;
    inhost = casterinhost;
    inport = casterinport;
  }

  while (inputmode != LAST) {
    int input_init = 1;
    if (sigint_received)
      break;
    /*** InputMode handling ***/
    switch (inputmode) {
      case INFILE: {
        if ((gps_file = open(filepath, O_RDONLY)) < 0) {
          flag_io_error("ERROR: opening input file");
          exit(IO_ERROR);
        }
#ifndef WINDOWSVERSION
        /* set blocking inputmode in case it was not set
         (seems to be sometimes for fifo's) */
        fcntl(gps_file, F_SETFL, 0);
#endif
        printf("file input: file = %s\n", filepath);
      }
        break;
      case SERIAL: /* open serial port */  {
#ifndef WINDOWSVERSION
        gps_serial = openserial(ttyport, 1, ttybaud);
#else
        gps_serial = openserial(ttyport, ttybaud);
#endif
        if (gps_serial == INVALID_HANDLE_VALUE)
          exit(IO_ERROR);
        printf("serial input: device = %s, speed = %d\n", ttyport, ttybaud);

        if (initfile) {
          char buffer[1024];
          FILE *fh;
          int i;

          if ((fh = fopen(initfile, "r"))) {
            while ((i = fread(buffer, 1, sizeof(buffer), fh)) > 0) {
#ifndef WINDOWSVERSION
              if ((write(gps_serial, buffer, i)) != i) {
                flag_io_error("WARNING: sending init file");
                input_init = 0;
                break;
              }
#else
              DWORD nWrite = -1;
              if(!WriteFile(gps_serial, buffer, sizeof(buffer), &nWrite, NULL))  {
                flag_io_error("ERROR: sending init file");
                input_init = 0;
                break;
              }
              i = (int)nWrite;
#endif
            }
            if (i < 0) {
              flag_io_error("ERROR: reading init file");
              reconnect_sec_max = 0;
              input_init = 0;
              break;
            }
            fclose(fh);
          } else {
            flag_str_error("ERROR: can't read init file <%s>", initfile);
            reconnect_sec_max = 0;
            input_init = 0;
            break;
          }
        }
      }
        break;
      case TCPSOCKET:
      case UDPSOCKET:
      case SISNET:
      case NTRIP1_IN:
      case NTRIP2_HTTP_IN: {
        if (inputmode == SISNET) {
          if (!inhost)
            inhost = SISNET_SERVER;
          if (!inport)
            inport = SISNET_PORT;
        } else if (inputmode == NTRIP1_IN || inputmode == NTRIP2_HTTP_IN) {
          if (!inport)
            inport = NTRIP_PORT;
          if (!inhost)
            inhost = NTRIP_CASTER;
        } else if ((inputmode == TCPSOCKET) || (inputmode == UDPSOCKET)) {
          if (!inport)
            inport = SERV_TCP_PORT;
          if (!inhost)
            inhost = SERV_HOST_ADDR;
        }

        if (!(he = gethostbyname(inhost))) {
          flag_str_error("ERROR: Input host <%s> unknown", inhost);
          exit(IO_ERROR);
        }

        if ((gps_socket = socket(AF_INET, inputmode == UDPSOCKET ? SOCK_DGRAM : SOCK_STREAM, 0)) == INVALID_SOCKET) {
          flag_socket_error("ERROR: can't create socket for incoming data stream");
          exit(IO_ERROR);
        }

        memset((char*) &caster, 0x00, sizeof(caster));
        if (!bindmode)
          memcpy(&caster.sin_addr, he->h_addr, (size_t)he->h_length);
        caster.sin_family = AF_INET;
        caster.sin_port = htons(inport);

        if (bindmode) {
          if (bind(gps_socket, (struct sockaddr*) &caster, sizeof(caster))
              < 0) {
            flag_int_error("ERROR: can't bind input to port %d", inport);
            reconnect_sec_max = 0;
            input_init = 0;
            break;
          }
        } /* connect to input-caster or proxy server*/
        else if (connect(gps_socket, (struct sockaddr*) &caster, sizeof(caster)) < 0) {
          snprintf(msgbuf, sizeof(msgbuf), "WARNING: can't connect input to %s at port %d\n",
                   inet_ntoa(caster.sin_addr), inport);
          flag_logical_error(msgbuf);
          input_init = 0;
          break;
        }

        /* input from NTRIP caster */
        if (stream_name) {
          int init = 0;
          /* set socket buffer size */
          setsockopt(gps_socket, SOL_SOCKET, SO_SNDBUF, (const char*) &size, sizeof(const char*));
          /* input from Ntrip caster*/
          nBufferBytes=snprintf(szSendBuffer, sizeof(szSendBuffer) - 40,/* leave some space for login */
          "GET %s/%s HTTP/1.1\r\n"
          "Host: %s\r\n"
          "%s"
          "User-Agent: %s/%s\r\n"
          //"%s%s%s"        // nmea
          "Connection: close%s",
          get_extension,
          stream_name ? stream_name : "",
          casterinhost,
          inputmode == NTRIP1_IN ? "" : "Ntrip-Version: Ntrip/2.0\r\n",
          AGENTSTRING, revisionstr,
          //args.nmea ? "Ntrip-GGA: " : "", args.nmea ? args.nmea : "", args.nmea ? "\r\n" : "", // TODO: add argument
          (*stream_user || *stream_password) ? "\r\nAuthorization: Basic " : "");
          /* second check for old glibc */
          if (nBufferBytes > (int) sizeof(szSendBuffer) - 40 || nBufferBytes < 0) {
            flag_logical_error("ERROR: Source caster request too long");
            input_init = 0;
            reconnect_sec_max = 0;
            break;
          }
          nBufferBytes += encode(szSendBuffer + nBufferBytes, sizeof(szSendBuffer) - nBufferBytes - 4,
                                 stream_user, stream_password);
          if (nBufferBytes > (int) sizeof(szSendBuffer) - 4) {
            flag_logical_error("ERROR: Source caster user ID and/or password too long");
            input_init = 0;
            reconnect_sec_max = 0;
            break;
          }
          szSendBuffer[nBufferBytes++] = '\r';
          szSendBuffer[nBufferBytes++] = '\n';
          szSendBuffer[nBufferBytes++] = '\r';
          szSendBuffer[nBufferBytes++] = '\n';
  #ifndef NDEBUG
          printf("%s\n", szSendBuffer);
  #endif
          if ((send(gps_socket, szSendBuffer, (size_t) nBufferBytes, 0)) != nBufferBytes) {
            flag_socket_error("WARNING: could not send Source caster request");
            input_init = 0;
            break;
          }
          nBufferBytes = 0;
          /* check Source caster's response */
          while (!init && nBufferBytes < (int) sizeof(szSendBuffer) &&
                 (nBufferBytes += recv(gps_socket, szSendBuffer+nBufferBytes,  sizeof(szSendBuffer) - nBufferBytes, 0)) > 0) {
            if ((nBufferBytes > 17) && !strstr(szSendBuffer, "ICY 200 OK")  &&  /* case 'proxy & ntrip 1.0 caster' */
              (!strncmp(szSendBuffer, "HTTP/1.1 200 OK\r\n", 17) ||
               !strncmp(szSendBuffer, "HTTP/1.0 200 OK\r\n", 17)) ) {
              const char *datacheck   = "Content-Type: gnss/data\r\n";
              const char *chunkycheck = "Transfer-Encoding: chunked\r\n";
              int l = strlen(datacheck)-1;
              int j=0;
              for(i = 0; j != l && i < nBufferBytes-l; ++i)  {
                for(j = 0; j < l && szSendBuffer[i+j] == datacheck[j]; ++j)
                  ;
              }
              if(i == nBufferBytes-l) {
                flag_logical_error("No 'Content-Type: gnss/data' found");
                input_init = 0;
              }
              l = strlen(chunkycheck)-1;
              j=0;
              for(i = 0; j != l && i < nBufferBytes-l; ++i) {
                for(j = 0; j < l && szSendBuffer[i+j] == chunkycheck[j]; ++j)
                  ;
              }
              if(i < nBufferBytes-l)
                chunkymode = 1;
              init = 1;
            } // if( nBufferBytes > 17 && !strstr(szSendBuffer, "ICY 200 OK")  &&
            else if (strstr(szSendBuffer, "\r\n")) {
              if (!strstr(szSendBuffer, "ICY 200 OK")) {
                msglen = snprintf(msgbuf, sizeof(msgbuf), "ERROR: could not get requested data from Source caster: ");
                str_as_printable(szSendBuffer, nBufferBytes, msgbuf+msglen, sizeof(msgbuf)-msglen);
                flag_logical_error(msgbuf);
                if (!strstr(szSendBuffer, "SOURCETABLE 200 OK")) {
                  reconnect_sec_max = 0;
                }
                input_init = 0;
                break;
              }
              init = 1;
            } // if( nBufferBytes > 17 && !strstr(szSendBuffer, "ICY 200 OK")
          } // while (!init && nBufferBytes < (int) sizeof(szSendBuffer) &&
        } // if (stream_name)

        if (initfile && inputmode != SISNET) {
          char buffer[1024];
          FILE *fh;
          int i;

          if ((fh = fopen(initfile, "r"))) {
            while ((i = fread(buffer, 1, sizeof(buffer), fh)) > 0) {
              if ((send(gps_socket, buffer, (size_t) i, 0)) != i) {
                flag_socket_error("WARNING: sending init file");
                input_init = 0;
                break;
              }
            }
            if (i < 0) {
              flag_io_error("ERROR: reading init file");
              reconnect_sec_max = 0;
              input_init = 0;
              break;
            }
            fclose(fh);
          } else {
            flag_str_error("ERROR: can't read init file <%s>", initfile);
            reconnect_sec_max = 0;
            input_init = 0;
            break;
          }
        } // if (initfile && inputmode != SISNET)
      } // case NTRIP2_HTTP_IN:
        if (inputmode == SISNET) {
          int i, j;
          char buffer[1024];

          i = snprintf(buffer, sizeof(buffer),
              sisnet >= 30 ? "AUTH,%s,%s\r\n" : "AUTH,%s,%s", sisnetuser,
              sisnetpassword);
          if ((send(gps_socket, buffer, (size_t) i, 0)) != i) {
            flag_socket_error("WARNING: sending authentication for SISNeT data server");
            input_init = 0;
            break;
          }
          i = sisnet >= 30 ? 7 : 5;
          if ((j = recv(gps_socket, buffer, i, 0)) != i
              && strncmp("*AUTH", buffer, 5)) {
            msglen = snprintf(msgbuf, sizeof(msgbuf), "WARNING: SISNeT connect failed: ");
            str_as_printable(buffer, j, msgbuf+msglen, sizeof(msgbuf)-msglen);
            flag_logical_error(msgbuf);
            input_init = 0;
            break;
          }
          if (sisnet >= 31) {
            if ((send(gps_socket, "START\r\n", 7, 0)) != i) {
              flag_socket_error("WARNING: sending Sisnet start command");
              input_init = 0;
              break;
            }
          }
        } // if (inputmode == SISNET)
        /*** receiver authentication  ***/
        if (recvrid && recvrpwd
            && ((inputmode == TCPSOCKET) || (inputmode == UDPSOCKET))) {
          if (strlen(recvrid) > (BUFSZ - 3)) {
            flag_logical_error("ERROR: Receiver ID too long");
            reconnect_sec_max = 0;
            input_init = 0;
            break;
          } else {
            flag_logical_error("Sending user ID for receiver...");
            nBufferBytes = recv(gps_socket, szSendBuffer, BUFSZ, 0);
            strcpy(szSendBuffer, recvrid);
            strcat(szSendBuffer, "\r\n");
            if (send(gps_socket, szSendBuffer, strlen(szSendBuffer),
                MSG_DONTWAIT) < 0) {
              flag_socket_error("WARNING: sending user ID for receiver");
              input_init = 0;
              break;
            }
          } // if (strlen(recvrid) > (BUFSZ - 3))

          if (strlen(recvrpwd) > (BUFSZ - 3)) {
            flag_logical_error("ERROR: Receiver password too long");
            reconnect_sec_max = 0;
            input_init = 0;
            break;
          } else {
            flag_logical_error("Sending user password for receiver...\n");
            nBufferBytes = recv(gps_socket, szSendBuffer, BUFSZ, 0);
            strcpy(szSendBuffer, recvrpwd);
            strcat(szSendBuffer, "\r\n");
            if (send(gps_socket, szSendBuffer, strlen(szSendBuffer),
                MSG_DONTWAIT) < 0) {
              flag_socket_error("WARNING: sending user password for receiver");
              input_init = 0;
              break;
            }
          } // if (strlen(recvrpwd) > (BUFSZ - 3))
        } //if (recvrid && recvrpwd

        snprintf(msgbuf, sizeof(msgbuf), "%d", proxyport);
        printf("input %sconnected to %s://%s:%d%s%s%s%s%s%s%s%s%s\n",
            input_init ? "" : "NOT ",
            inputmode == NTRIP1_IN ? "ntrip1" :
            inputmode == NTRIP2_HTTP_IN ? "ntrip2" :
            inputmode == SISNET ? "sisnet" :
            inputmode == TCPSOCKET ? "tcpserver" : "udp",
            //bindmode ? "127.0.0.1" : inet_ntoa(caster.sin_addr),
            casterinhost, casterinport,
            stream_name ? "/" : "", stream_name ? stream_name : "",
            initfile ? ", initfile=" : "", initfile ? initfile : "",
            bindmode ? ", binding mode" : "",
            *proxyhost ? ", proxy=" : "", proxyhost,
            *proxyhost ? ":" : "", *proxyhost ? msgbuf : "");

        break;
      default:
        flag_int_error("ERROR: unknown input mode %d", inputmode);
        exit(CMD_VALUE_ERROR);
        break;
    }; // switch (inputmode)

    /* ----- main part ----- */
    int output_init = 1, fallback = 0;

    while ((input_init) && (output_init)) {
#ifndef WINDOWSVERSION
      if ((sigalarm_received) || (sigint_received) || (sigpipe_received))
        break;
#else
      if((sigalarm_received) || (sigint_received)) break;
#endif

      if (!(he = gethostbyname(outhost))) {
        flag_str_error("ERROR: Destination caster, server or proxy host <%s> unknown", outhost);
        close_session(casterouthost, mountpoint, session, rtsp_extension, 0);
        exit(IO_ERROR);
      } else {
        //printf("Destination caster, server or proxy host <%s>\n", outhost);
      }

      /* create socket */
      if ((socket_tcp = socket(AF_INET, (outputmode == UDP ? SOCK_DGRAM : SOCK_STREAM), 0)) == INVALID_SOCKET) {
        flag_socket_error("ERROR: tcp socket");
        reconnect_sec_max = 0;
        break;
      }

      {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (setsockopt(socket_tcp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
          flag_socket_error("ERROR: setsockopt SO_RCVTIMEO");
          break;
        }
      }

      if (outputmode == TCPIP) {
        // Forcefully attaching socket to the local port
        int opt = 1;
        if (setsockopt(socket_tcp, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                                                      &opt, sizeof(opt)))  {
          flag_socket_error("ERROR: setsockopt SO_REUSEADDR | SO_REUSEPORT");
          break;
        }
      }
      memset((char*) &caster, 0x00, sizeof(caster));
      memcpy(&caster.sin_addr, he->h_addr, (size_t)he->h_length);
      caster.sin_family = AF_INET;
      caster.sin_port = htons(outport);

      /*** setup currentoutputmode ***/
      currentoutputmode = outputmode;
      if (useNTRIP1) {
         useNTRIP1 = 0;
         currentoutputmode = NTRIP1;
      }

      if (currentoutputmode == TCPIP) {
        caster.sin_addr.s_addr = INADDR_ANY;
        // Forcefully attaching socket to the local port
        if (bind(socket_tcp, (struct sockaddr *)&caster, sizeof(caster)) < 0)        {
            flag_socket_error("ERROR: bind failed");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
        }
        if (listen(socket_tcp, 3) < 0) {
            flag_socket_error("listen");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
        }
        int addrlen = sizeof(caster);
        if ((local_socket_tcp = accept(socket_tcp, (struct sockaddr *)&caster,
                                      (socklen_t*)&addrlen)) < 0) {
            flag_socket_error("ERROR: accept");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
        }
      } // if (currentoutputmode == TCPIP)
      else {
        if (connect(socket_tcp, (struct sockaddr*) &caster, sizeof(caster)) < 0) {
          snprintf(msgbuf, sizeof(msgbuf), "WARNING: can't connect output to %s at port %d",
                   inet_ntoa(caster.sin_addr), outport);
          flag_logical_error(msgbuf);
          break;
        }
      } // if (currentoutputmode == TCPIP)

      /* connect to Destination caster, server or proxy host */
      printf("TCP connected to %s:%d\n",
            casterouthost, casteroutport);

      /*** OutputMode handling ***/
      switch (currentoutputmode) {
        case UDP: {
          unsigned int session;
          char rtpbuf[1526];
          int i = 12, j;

          udp_init = time(0);
          srand(udp_init);
          session = rand();
          udp_tim = rand();
          udp_seq = rand();

          rtpbuf[0] = (2 << 6);
          /* padding, extension, csrc are empty */
          rtpbuf[1] = 97;
          /* marker is empty */
          rtpbuf[2] = (udp_seq >> 8) & 0xFF;
          rtpbuf[3] = (udp_seq) & 0xFF;
          rtpbuf[4] = (udp_tim >> 24) & 0xFF;
          rtpbuf[5] = (udp_tim >> 16) & 0xFF;
          rtpbuf[6] = (udp_tim >> 8) & 0xFF;
          rtpbuf[7] = (udp_tim) & 0xFF;
          /* sequence and timestamp are empty */
          rtpbuf[8] = (session >> 24) & 0xFF;
          rtpbuf[9] = (session >> 16) & 0xFF;
          rtpbuf[10] = (session >> 8) & 0xFF;
          rtpbuf[11] = (session) & 0xFF;
          ++udp_seq;

          j = snprintf(rtpbuf + i, sizeof(rtpbuf) - i - 40, /* leave some space for login */
          "POST /%s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Ntrip-Version: Ntrip/2.0\r\n"
              "User-Agent: %s/%s\r\n"
              "Authorization: Basic %s%s%s\r\n"
              "Connection: close\r\n"
              "Transfer-Encoding: chunked\r\n\r\n", mountpoint, casterouthost,
          AGENTSTRING, revisionstr, authorization,
              ntrip_str ?
                  (outputmode == NTRIP1 ? "\r\nSTR: " : "\r\nNtrip-STR: ") : "",
              ntrip_str);
          i += j;
          if (i > (int) sizeof(rtpbuf) - 40 || j < 0) /* second check for old glibc */
          {
            flag_logical_error("Requested data too long");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
          } else {
            rtpbuf[i++] = '\r';
            rtpbuf[i++] = '\n';
            rtpbuf[i++] = '\r';
            rtpbuf[i++] = '\n';

            if (send(socket_tcp, rtpbuf, i, 0) != i) {
              flag_socket_error("Could not send UDP packet");
              reconnect_sec_max = 0;
              output_init = 0;
              break;
            } else {
              int stop = 0;
              int numbytes = recv_from_caster(rtpbuf, sizeof(rtpbuf)-1, msgbuf, sizeof(msgbuf), "NTRIPv2 UDP");
              if (!*msgbuf){
                /* we don't expect message longer than 1513, so we cut the last
                 byte for security reasons to prevent buffer overrun */
                rtpbuf[numbytes] = 0;
                if (numbytes > 17 + 12
                    && (!strncmp(rtpbuf + 12, "HTTP/1.1 200 OK\r\n", 17)
                        || !strncmp(rtpbuf + 12, "HTTP/1.0 200 OK\r\n", 17))) {
                  const char *sessioncheck = "session: ";
                  int l = strlen(sessioncheck) - 1;
                  int j = 0;
                  for (i = 12; j != l && i < numbytes - l; ++i) {
                    for (j = 0;
                        j < l && tolower(rtpbuf[i + j]) == sessioncheck[j]; ++j)
                      ;
                  }
                  if (i != numbytes - l) /* found a session number */
                  {
                    i += l;
                    session = 0;
                    while (i < numbytes && rtpbuf[i] >= '0' && rtpbuf[i] <= '9')
                      session = session * 10 + rtpbuf[i++] - '0';
                    if (rtpbuf[i] != '\r') {
                      flag_logical_error("Could not extract session number");
                      stop = 1;
                    }
                  }
                } else {
                  msglen = snprintf(msgbuf, sizeof(msgbuf), "Could not access mountpoint: ");
                  str_as_printable(rtpbuf+12, numbytes-12, msgbuf+msglen, sizeof(msgbuf)-msglen);
                }
              } // if (!*msgbuf)
              if (*msgbuf){
                flag_logical_error(msgbuf);
                stop = 1;
              } // if (output_init)
              if (!stop) {
                send_receive_loop(socket_tcp, outputmode, NULL, 0, session, chunkymode);
                input_init = output_init = 0;
                /* send connection close always to allow nice session closing */
                udp_tim += (time(0) - udp_init) * 1000000 / TIME_RESOLUTION;
                rtpbuf[0] = (2 << 6);
                /* padding, extension, csrc are empty */
                rtpbuf[1] = 98;
                /* marker is empty */
                rtpbuf[2] = (udp_seq >> 8) & 0xFF;
                rtpbuf[3] = (udp_seq) & 0xFF;
                rtpbuf[4] = (udp_tim >> 24) & 0xFF;
                rtpbuf[5] = (udp_tim >> 16) & 0xFF;
                rtpbuf[6] = (udp_tim >> 8) & 0xFF;
                rtpbuf[7] = (udp_tim) & 0xFF;
                /* sequence and timestamp are empty */
                rtpbuf[8] = (session >> 24) & 0xFF;
                rtpbuf[9] = (session >> 16) & 0xFF;
                rtpbuf[10] = (session >> 8) & 0xFF;
                rtpbuf[11] = (session) & 0xFF;

                send(socket_tcp, rtpbuf, 12, 0); /* cleanup */
              } else {
                reconnect_sec_max = 600;
                output_init = 0;
              } /* if (!stop) */
            } /* if (send(socket_tcp, rtpbuf, i, 0) != i) */
          } /* if (i > (int) sizeof(rtpbuf) - 40 || j < 0) */
        }
        break; /* case UDP */
        case NTRIP1: /*** OutputMode Ntrip Version 1.0 ***/
          fallback = 0;
          nrip1Mountpoint = strstr(casterouthost,"onocoy.com") ? user : mountpoint;
          nBufferBytes = snprintf(szSendBuffer, sizeof(szSendBuffer),
              "SOURCE %s %s\r\nSource-Agent: %s/%s\r\nSTR: \r\n\r\n",
              password, nrip1Mountpoint,
              AGENTSTRING, revisionstr);
          if ((nBufferBytes > (int) sizeof(szSendBuffer))
              || (nBufferBytes < 0)) {
            flag_logical_error("ERROR: Destination caster request to long");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
          }
          if (!send_to_caster(szSendBuffer, socket_tcp, nBufferBytes)) {
            output_init = 0;
            break;
          }
          /* check Destination caster's response */
          nBufferBytes = recv_from_caster(szSendBuffer, sizeof(szSendBuffer), msgbuf, sizeof(msgbuf), "NTRIPv1");
          if (!*msgbuf) {
#ifndef NDEBUG
            printf("Caster response: %s\n", szSendBuffer);
#endif
            if (strstr(szSendBuffer, NTRIPv1_RSP_OK_SVR) || strstr(szSendBuffer, CODE_RSP_OK_SVR)) {
              //printf("NTRIPv1 server OK for %s:%d/%s\n", casterouthost, casteroutport, nrip1Mountpoint);
            } else if (nBufferBytes == 0) { /* buffer epmpty */
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server NO ANY response from %s:%d/%s",
                       casterouthost, casteroutport, nrip1Mountpoint);
            } else if (strstr(szSendBuffer, NTRIPv1_RSP_NOT_FOUND) || strstr(szSendBuffer, NTRIPv2_RSP_NOT_FOUND) || strstr(szSendBuffer, CODE_RSP_NOT_FOUND)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server mountpoint is INVALID for %s:%d/%s",
                       casterouthost, casteroutport, nrip1Mountpoint);
              set_reconnect_time_at_hard_error();
            } else if (strstr(szSendBuffer, NTRIPv1_RSP_UNAUTH) || strstr(szSendBuffer, NTRIPv2_RSP_UNAUTH) || strstr(szSendBuffer, CODE_RSP_UNAUTH)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server password is INVALID for %s:%d/%s",
                       casterouthost, casteroutport, nrip1Mountpoint);
              set_reconnect_time_at_hard_error();
            } else if (strstr(szSendBuffer, NTRIPv1_RSP_USED2)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server mountpoint is ALREADY USED or INVALID for %s:%d/%s",
                       casterouthost, casteroutport, nrip1Mountpoint);
            } else if (strstr(szSendBuffer, NTRIPv1_RSP_USED1) || strstr(szSendBuffer, NTRIPv1_RSP_USED2) || strstr(szSendBuffer, NTRIPv2_RSP_USED) || strstr(szSendBuffer, CODE_RSP_USED)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server mountpoint is ALREADY USED for %s:%d/%s",
                       casterouthost, casteroutport, nrip1Mountpoint);
            } else if (strstr(szSendBuffer, NTRIPv1_RSP_ERROR) || strstr(szSendBuffer, NTRIPv2_RSP_ERROR) || strstr(szSendBuffer, CODE_RSP_ERROR)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server ERROR for %s:%d/%s",
                                    casterouthost, casteroutport, nrip1Mountpoint);
            } else if (nBufferBytes >= NTRIP_MAXRSP) { /* buffer overflow */
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server response OVERFLOW from %s:%d/%s",
                       casterouthost, casteroutport, nrip1Mountpoint);
            } else {
              msglen = snprintf(msgbuf, sizeof(msgbuf), "NTRIPv1 server is NOT OK for %s:%d/%s: ",
                                casterouthost, casteroutport, nrip1Mountpoint);
              str_as_printable(szSendBuffer, nBufferBytes, msgbuf, sizeof(msgbuf));
            }
          } // if (!*msgbuf)
          if (*msgbuf){
            flag_logical_error(msgbuf);
            output_init = 0;
            break;
          } // if (output_init)

          send_receive_loop(socket_tcp, currentoutputmode, NULL, 0, 0, chunkymode);
          input_init = output_init = 0;
          break; /* case NTRIP1 */
        case HTTP: /*** Ntrip-Version 2.0 HTTP/1.1 ***/
          nBufferBytes = snprintf(szSendBuffer, sizeof(szSendBuffer),
              "POST %s/%s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "Ntrip-Version: Ntrip/2.0\r\n"
                  "User-Agent: %s/%s\r\n"
                  "Authorization: Basic %s%s%s\r\n"
                  "Connection: close\r\n"
                  "%s\r\n", post_extension,
              mountpoint, casterouthost, AGENTSTRING, revisionstr,
              authorization, ntrip_str ? "\r\nNtrip-STR: " : "", ntrip_str,
              output_v2http_chunken ? "Transfer-Encoding: chunked\r\n" : "");
          if ((nBufferBytes > (int) sizeof(szSendBuffer))
              || (nBufferBytes < 0)) {
            flag_logical_error("ERROR: Destination caster request to long");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
          }
          if (!send_to_caster(szSendBuffer, socket_tcp, nBufferBytes)) {
            output_init = 0;
            break;
          }
          /* check Destination caster's response */
          nBufferBytes = recv_from_caster(szSendBuffer, sizeof(szSendBuffer), msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP");
          if (!*msgbuf) {
#ifndef NDEBUG
            printf("Caster response: %s\n", szSendBuffer);
#endif
            if (strstr(szSendBuffer, NTRIPv2_RSP_OK_SVR) || strstr(szSendBuffer, CODE_RSP_OK_SVR)) {
              //printf("NTRIPv2 server OK for %s:%d/%s\n", casterouthost, casteroutport, mountpoint);
            } else if (nBufferBytes == 0) { /* buffer epmpty */
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP NO ANY response from %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
            } else if (strstr(szSendBuffer, NTRIPv2_RSP_NOT_FOUND) || strstr(szSendBuffer, CODE_RSP_NOT_FOUND)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP mountpoint is INVALID for %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
              set_reconnect_time_at_hard_error();
            } else if (strstr(szSendBuffer, NTRIPv2_RSP_UNAUTH) || strstr(szSendBuffer, CODE_RSP_UNAUTH)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP login/password is INVALID for %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
              set_reconnect_time_at_hard_error();
            } else if (strstr(szSendBuffer, NTRIPv2_RSP_USED) || strstr(szSendBuffer, CODE_RSP_USED)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP mountpoint is ALREADY USED for %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
            } else if (strstr(szSendBuffer, NTRIPv2_RSP_NOTIMPL) || strstr(szSendBuffer, CODE_RSP_NOTIMPL)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP NOT IMLEMENTED for %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
              useNTRIP1 = 1;
            } else if (strstr(szSendBuffer, NTRIPv2_RSP_UNAVAIL) || strstr(szSendBuffer, CODE_RSP_UNAVAIL)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP UNAVAILABLE for %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
            } else if (strstr(szSendBuffer, NTRIPv1_RSP_ERROR) || strstr(szSendBuffer, NTRIPv2_RSP_ERROR) || strstr(szSendBuffer, CODE_RSP_ERROR)) {
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP server ERROR for %s:%d/%s",
                                    casterouthost, casteroutport, mountpoint);
            } else if (nBufferBytes >= NTRIP_MAXRSP) { /* buffer overflow */
              snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 response OVERFLOW from %s:%d/%s",
                       casterouthost, casteroutport, mountpoint);
            } else {
              msglen = snprintf(msgbuf, sizeof(msgbuf), "NTRIPv2 HTTP server%s is NOT OK for %s:%d/%s: ",
                                *proxyhost ? " or Proxy's" : "", casterouthost, casteroutport, mountpoint);
              str_as_printable(szSendBuffer, nBufferBytes, msgbuf, sizeof(msgbuf));
              if (!strstr(szSendBuffer, NTRIPv2_RSP_VERSION))
                 useNTRIP1 = 1;
            }
          } // if (!*msgbuf)
          if (*msgbuf){
            flag_logical_error(msgbuf);
            if (useNTRIP1) {
              snprintf(msgbuf, sizeof(msgbuf),
                       "NTRIP 2.0 HTTP not implemented at <%s>%s%s%s falls back to NTRIP 1.0",
                       casterouthost, *proxyhost ? " or Proxy <" : "", proxyhost,
                       *proxyhost ? "> or HTTP/1.1 not implemented at Proxy" : "");
              flag_logical_error(msgbuf);
              close_session(casterouthost, mountpoint, session, rtsp_extension, 1);
            }
            output_init = 0;
            break;
          } // if (output_init)
          send_receive_loop(socket_tcp, outputmode, NULL, 0, 0, chunkymode);
          input_init = output_init = 0;
          break; /* case HTTP */
        case RTSP: /*** Ntrip-Version 2.0 RTSP / RTP ***/
          if ((socket_udp = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
            flag_socket_error("ERROR: udp socket");
            exit(IO_ERROR);
          }
          /* fill structure with local address information for UDP */
          memset(&local, 0, sizeof(local));
          local.sin_family = AF_INET;
          local.sin_port = htons(0);
          local.sin_addr.s_addr = htonl(INADDR_ANY);
          len = (socklen_t) sizeof(local);
          /* bind() in order to get a random RTP client_port */
          if ((bind(socket_udp, (struct sockaddr*) &local, len)) < 0) {
            flag_socket_error("ERROR: udp bind");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
          }
          if ((getsockname(socket_udp, (struct sockaddr*) &local, &len)) != -1) {
            client_port = (unsigned int) ntohs(local.sin_port);
          } else {
            flag_socket_error("ERROR: getsockname(localhost)");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
          }
          nBufferBytes = snprintf(szSendBuffer, sizeof(szSendBuffer),
              "SETUP rtsp://%s%s/%s RTSP/1.0\r\n"
                  "CSeq: %d\r\n"
                  "Ntrip-Version: Ntrip/2.0\r\n"
                  "Ntrip-Component: Ntripserver\r\n"
                  "User-Agent: %s/%s\r\n"
                  "Transport: RTP/GNSS;unicast;client_port=%u\r\n"
                  "Authorization: Basic %s%s%s\r\n\r\n", casterouthost,
              rtsp_extension, mountpoint, udp_cseq++, AGENTSTRING, revisionstr,
              client_port, authorization, ntrip_str ? "\r\nNtrip-STR: " : "",
              ntrip_str);
          if ((nBufferBytes > (int) sizeof(szSendBuffer))
              || (nBufferBytes < 0)) {
            flag_logical_error("ERROR: Destination caster request to long");
            reconnect_sec_max = 0;
            output_init = 0;
            break;
          }
          if (!send_to_caster(szSendBuffer, socket_tcp, nBufferBytes)) {
            output_init = 0;
            break;
          }

          while ((nBufferBytes = recv_from_caster(szSendBuffer,
              sizeof(szSendBuffer), msgbuf, sizeof(msgbuf), "NTRIPv2 RTSP")) > 0) {
            /* check Destination caster's response */
            if (*msgbuf)
               break;
            if (!strstr(szSendBuffer, "RTSP/1.0 200 OK")) {
              msglen = snprintf(msgbuf, sizeof(msgbuf), "ERROR: Destination caster's%s reply is not OK: ",
                  *proxyhost ? " or Proxy's" : "");
              str_as_printable(szSendBuffer, nBufferBytes, msgbuf+msglen, sizeof(msgbuf)-msglen);
              flag_logical_error(msgbuf);
              *msgbuf = 0;
              /* fallback if necessary */
              if (strncmp(szSendBuffer, "RTSP", 4) != 0) {
                if (strstr(szSendBuffer, "Ntrip-Version: Ntrip/2.0\r\n")) {
                  snprintf(msgbuf, sizeof(msgbuf),
                      "       RTSP not implemented at Destination caster <%s>%s%s%s"
                          "ntripserver falls back to Ntrip Version 2.0 in TCP/IP mode", casterouthost,
                      *proxyhost ? " or Proxy <" : "", proxyhost,
                      *proxyhost ? ">" : "");
                  flag_logical_error(msgbuf);
                  *msgbuf = 0;
                  close_session(casterouthost, mountpoint, session,
                      rtsp_extension, 1);
                  outputmode = HTTP;
                  fallback = 1;
                  break;
                } else {
                  snprintf(msgbuf, sizeof(msgbuf),
                      "NTRIP 2.0 RSTP not implemented at <%s>%s%s%s %s or RTSP/1.0 not implemented%s falls back to NTRIP 1.0",
                      casterouthost, *proxyhost ? " or Proxy <" : "", proxyhost,
                      *proxyhost ? ">" : "",
                      *proxyhost ? " or HTTP/1.1 not implemented at Proxy\n" : "",
                      *proxyhost ? " or Proxy" : "");
                  flag_logical_error(msgbuf);
                  *msgbuf = 0;
                  close_session(casterouthost, mountpoint, session,
                      rtsp_extension, 1);
                  useNTRIP1 = 1;
                  fallback = 1;
                  break;
                }
              } else if ((strstr(szSendBuffer, "RTSP/1.0 401 Unauthorized"))
                  || (strstr(szSendBuffer, "RTSP/1.0 501 Not Implemented"))) {
                reconnect_sec_max = 0;
              }
              output_init = 0;
              break;
            }
#ifndef NDEBUG
            else {
              printf("Destination caster response:\n%s\n", szSendBuffer);
            }
#endif
            if ((strstr(szSendBuffer, "RTSP/1.0 200 OK\r\n"))
                && (strstr(szSendBuffer, "CSeq: 1\r\n"))) {
              for (token = strtok(szSendBuffer, dlim); token != NULL; token =
                  strtok(NULL, dlim)) {
                tok_buf[i] = token;
                i++;
              }
              session = atoi(tok_buf[6]);
              server_port = atoi(tok_buf[10]);
              nBufferBytes = snprintf(szSendBuffer, sizeof(szSendBuffer),
                  "RECORD rtsp://%s%s/%s RTSP/1.0\r\n"
                      "CSeq: %d\r\n"
                      "Session: %u\r\n"
                      "\r\n", casterouthost, rtsp_extension, mountpoint,
                  udp_cseq++, session);
              if ((nBufferBytes >= (int) sizeof(szSendBuffer))
                  || (nBufferBytes < 0)) {
                flag_logical_error("ERROR: Destination caster request to long");
                reconnect_sec_max = 0;
                output_init = 0;
                break;
              }
              if (!send_to_caster(szSendBuffer, socket_tcp, nBufferBytes)) {
                output_init = 0;
                break;
              }
            } else if ((strstr(szSendBuffer, "RTSP/1.0 200 OK\r\n"))
                && (strstr(szSendBuffer, "CSeq: 2\r\n"))) {
              /* fill structure with caster address information for UDP */
              memset(&casterRTP, 0, sizeof(casterRTP));
              casterRTP.sin_family = AF_INET;
              casterRTP.sin_port = htons(((uint16_t) server_port));
              if ((he = gethostbyname(outhost)) == NULL) {
                flag_io_error("ERROR: Destination caster unknown");
                reconnect_sec_max = 0;
                output_init = 0;
                break;
              } else {
                memcpy((char*) &casterRTP.sin_addr.s_addr, he->h_addr_list[0],
                    (size_t) he->h_length);
              }
              len = (socklen_t) sizeof(casterRTP);
              send_receive_loop(socket_udp, outputmode,
                  (struct sockaddr*) &casterRTP, (socklen_t) len, session, chunkymode);
              break;
            } else {
              break;
            }
          } /* while ((nBufferBytes = recv(socket_tcp, szSendBuffer */
          if (*msgbuf)
            flag_logical_error(msgbuf);
          input_init = output_init = 0;
          break; /* case RTSP */
        case TCPIP:
          fallback = 0;
          send_receive_loop(local_socket_tcp, outputmode, NULL, 0, 0, chunkymode);
          input_init = output_init = 0;
          break;
      } /* switch (outputmode) */
    } /* while ((input_init) && (output_init)) */
    close_session(casterouthost, mountpoint, session, rtsp_extension, 0);
    if ((reconnect_sec_max || fallback) && !sigint_received)
      reconnect_sec = reconnect(reconnect_sec, reconnect_sec_max);
    else
      inputmode = LAST;
  } /* while (inputmode != LAST) */
  return reconnect_sec_max ? SUCCESS : NET_ERROR;
} /* int main(int argc, char **argv) */

static ssize_t send_and_msg(int sock, const void *buffer, size_t length, int flags)
{
  int err;
  ssize_t i;
  char msg[200], recvbuf[8];
  i = send(sock, buffer, length, flags);
  if (i < 0) {
    err = errsock();
    if (err != EAGAIN) {
      if (err) {
         if ((err==EPIPE) && !recv(sock, recvbuf, 1, flags))
            sprintf(msg,"tcp server send disconnected by %s", outhost);
         else
             sprintf(msg,"tcp server send error %d (%s) at %s",err,errorstring(err), outhost);
      } else
          sprintf(msg,"tcp server send not all, disconnected by %s", outhost);
      flag_logical_error(msg);
    } else
      i = 0;
  }; // if (i < 0)
  return i;
}

#define SKIP_MAX_NO_RTCM3_PACKETS 3
static void send_receive_loop(sockettype sock, int outmode,
    struct sockaddr *pcasterRTP, socklen_t length, unsigned int rtpssrc,
    int chunkymode) {
  int nodata = 0;
  char buffer[BUFSZ] = { 0 };
  char sisnetbackbuffer[200];
  char szSendBuffer[BUFSZ] = "";
  int nBufferBytes = 0;
  int remainChunk = 0;

  /* RTSP / RTP Mode */
  int isfirstpacket = 1;
  struct timeval now;
  struct timeval last = { 0, 0 };
  long int sendtimediff;
  int rtpseq = 0;
  int rtptime = 0;
  time_t laststate = time(0);
  int firstInputRtcm3 = SKIP_MAX_NO_RTCM3_PACKETS;

  const char *actualMountpoint = (currentoutputmode == NTRIP1) ? nrip1Mountpoint : mountpoint;
  printf("NTRIP connected to %s://%s:%d/%s\n",
         currentoutputmode == NTRIP1 ? "ntrip1" :
         (currentoutputmode == HTTP) && !output_v2http_chunken ? "ntrip2_http"   :
         (currentoutputmode == HTTP) && output_v2http_chunken ? "ntrip2_chunken_http"   :
         currentoutputmode == UDP    ? "ntrip2_udp"    :
         currentoutputmode == RTSP   ? "ntrip2_rtsp"   : "tcpip",
         casterouthost, casteroutport, actualMountpoint);
  flag_erase();
  was_1st_ntrip_connect = 1;

  if (outmode == UDP) {
    rtptime = time(0);
#ifdef WINDOWSVERSION
    u_long blockmode = 1;
    if(ioctlsocket(socket_tcp, FIONBIO, &blockmode))
#else /* WINDOWSVERSION */
    if (fcntl(socket_tcp, F_SETFL, O_NONBLOCK) < 0)
#endif /* WINDOWSVERSION */
        {
      flag_socket_error("Could not set nonblocking mode");
      return;
    }
  } else if (outmode == RTSP) {
#ifdef WINDOWSVERSION
    u_long blockmode = 1;
    if(ioctlsocket(socket_tcp, FIONBIO, &blockmode))
#else /* WINDOWSVERSION */
    if (fcntl(socket_tcp, F_SETFL, O_NONBLOCK) < 0)
#endif /* WINDOWSVERSION */
        {
      flag_socket_error("Could not set nonblocking mode\n");
      return;
    }
  }

  /* data transmission */
  int send_recv_success = 0;
#ifdef WINDOWSVERSION
  time_t nodata_begin = 0, nodata_current = 0;
#endif
  while (1) {
    if (send_recv_success < 3)
      send_recv_success++;
    if (!nodata) {
#ifndef WINDOWSVERSION
      alarm(ALARMTIME);
#else
      time(&nodata_begin);
#endif
    } else {
      nodata = 0;
#ifdef WINDOWSVERSION
      time(&nodata_current);
      if(difftime(nodata_current, nodata_begin) >= ALARMTIME)  {
        sigalarm_received = 1;
        flag_int_error("ERROR: more than %d seconds no activity", ALARMTIME);
      }
#endif
    }
    /* signal handling*/
#ifdef WINDOWSVERSION
    if((sigalarm_received) || (sigint_received)) break;
#else
    if ((sigalarm_received) || (sigint_received) || (sigpipe_received))
      break;
#endif
    if (!nBufferBytes) {
      if (inputmode == SISNET && sisnet <= 30) {
        int i, j;
        /* a somewhat higher rate than 1 second to get really each block */
        /* means we need to skip double blocks sometimes */
        struct timeval tv = { 0, 700000 };
        select(0, 0, 0, 0, &tv);
        memcpy(sisnetbackbuffer, buffer, sizeof(sisnetbackbuffer));
        i = (sisnet >= 30 ? 5 : 3);

        if ((j=send_and_msg(gps_socket, "MSG\r\n", i, 0)) != i) {
          if (j >= 0)
             flag_socket_error("WARNING: sending SISNeT data request failed");
          return;
        }
      }
      /***********************/
      /* receiving data      */
      /***********************/

      /* INFILE  */
      if (inputmode == INFILE)
        nBufferBytes = read(gps_file, buffer, sizeof(buffer));

      /* SERIAL */
      else if (inputmode == SERIAL) {
#ifndef WINDOWSVERSION
        nBufferBytes = read(gps_serial, buffer, sizeof(buffer));
#else
        DWORD nRead = 0;
        if(!ReadFile(gps_serial, buffer, sizeof(buffer), &nRead, NULL))
        {
          flag_io_error("ERROR: reading serial input failed");
          return;
        }
        nBufferBytes = (int)nRead;
#endif
      }

      /* ALL OTHER MODES */
      else
#ifdef WINDOWSVERSION
        nBufferBytes = recv(gps_socket, buffer, sizeof(buffer), 0);
#else
        nBufferBytes = read(gps_socket, buffer, sizeof(buffer));
#endif

      if (!nBufferBytes) {
        //flag_socket_error("WARNING: no data received from input");
        nodata = 1;
#ifndef WINDOWSVERSION
        usleep(100000);
#else
        Sleep(100);
#endif
        continue;
      } else if ((nBufferBytes < 0) && (!sigint_received)) {
        flag_socket_error("WARNING: reading input failed");
        return;
      }
      /* we can compare the whole buffer, as the additional bytes
       remain unchanged */
      if (inputmode == SISNET && sisnet <= 30
          && !memcmp(sisnetbackbuffer, buffer, sizeof(sisnetbackbuffer))) {
        nBufferBytes = 0;
      }
    }
    if (nBufferBytes < 0)
      return;

    if (chunkymode) {
      int cstop = 0;
      int pos = 0;
      int totalbytes = 0;
      static int chunksize = 0;
      static long i = 0;
      char chunkBytes[BUFSZ] = { 0 };

      while (!sigint_received && !cstop && pos < nBufferBytes) {
        switch (chunkymode) {
          case 1: /* reading number starts */
            chunksize = 0;
            ++chunkymode; /* no break */
            break;
          case 2: /* during reading number */
            i = buffer[pos++];
            if (i >= '0' && i <= '9')
              chunksize = chunksize * 16 + i - '0';
            else if (i >= 'a' && i <= 'f')
              chunksize = chunksize * 16 + i - 'a' + 10;
            else if (i >= 'A' && i <= 'F')
              chunksize = chunksize * 16 + i - 'A' + 10;
            else if (i == '\r')
              ++chunkymode;
            else if (i == ';')
              chunkymode = 5;
            else
              cstop = 1;
            break;
          case 3: /* scanning for return */
            if (buffer[pos++] == '\n')
              chunkymode = chunksize ? 4 : 1;
            else
              cstop = 1;
            break;
          case 4: /* output data */
            i = nBufferBytes - pos;
            if (i > chunksize) {
              i = chunksize;
            }
            memcpy(chunkBytes + totalbytes, buffer + pos, (size_t) i);
            totalbytes += i;
            chunksize -= i;
            pos += i;
            if (!chunksize)
              chunkymode = 1;
            break;
          case 5:
            if (i == '\r')
              chunkymode = 3;
            break;
        }
      }
      if (cstop) {
        flag_logical_error("Error in chunky transfer encoding");
        return;
      }
      else {
        memset((char*) &buffer, 0x00, sizeof(buffer));
        memcpy(buffer, chunkBytes, (size_t) totalbytes);
        nBufferBytes = totalbytes;
      }
    }

    /****************************/
    /* control 1st RTCM3 packet */
    /****************************/
    if (firstInputRtcm3 && nBufferBytes) {
      //printf("firstInputRtcm3 nBufferBytes=%d\n",nBufferBytes);
      for (int i=0; i < (nBufferBytes-1); i++) {
         if ((buffer[i] == 0xD3) && ((buffer[i+1] & 0xFC) == 0)) {
           if ((firstInputRtcm3 != SKIP_MAX_NO_RTCM3_PACKETS) && (i != 0)) {
              int nPkt = (buffer[i+3] << 4) | (buffer[i+4] >> 4);
              printf("RTCM3 start at %d packet RTCM_%d pos=%d nBufferBytes=%d\n",
                     SKIP_MAX_NO_RTCM3_PACKETS - firstInputRtcm3 + 1, nPkt, i, nBufferBytes);
           }
           firstInputRtcm3 = 0;
           if (i > 0) {
              memmove(buffer, buffer + i, (size_t) (nBufferBytes - i));
              nBufferBytes -= i;
           }
           break;
         }
      }
      if (firstInputRtcm3) {
        firstInputRtcm3--;
        if (!firstInputRtcm3) printf("start at last packet\n");
        continue;
      }
    } // if (firstInputRtcm3 && nBufferBytes)

    /*****************/
    /*  send data    */
    /*****************/
    if (nBufferBytes && ((outmode == NTRIP1) || (outmode == TCPIP) || (!output_v2http_chunken && (outmode == HTTP)))) {
      int i;
      if ((i = send_and_msg(sock, buffer, (size_t) nBufferBytes, MSG_DONTWAIT)) != nBufferBytes) {
        if (i < 0) {
          return;
        } else if (i) {
          memmove(buffer, buffer + i, (size_t) (nBufferBytes - i));
          nBufferBytes -= i;
        }
      } else {
        nBufferBytes = 0;
      }
    } // if ((nBufferBytes) && (outmode == NTRIP1 || outmode == TCPIP))
    else if ((nBufferBytes) && (outmode == UDP)) {
      int i;
      char rtpbuf[1592];
      while(nBufferBytes)
      {
        int ct = time(0);
        int s = nBufferBytes;
        if(s > 1400)
          s = 1400;
        udp_tim += (ct - udp_init) * 1000000 / TIME_RESOLUTION;
        udp_init = ct;
        rtpbuf[0] = (2 << 6);
        rtpbuf[1] = 96;
        rtpbuf[2] = (udp_seq >> 8) & 0xFF;
        rtpbuf[3] = (udp_seq) & 0xFF;
        rtpbuf[4] = (udp_tim >> 24) & 0xFF;
        rtpbuf[5] = (udp_tim >> 16) & 0xFF;
        rtpbuf[6] = (udp_tim >> 8) & 0xFF;
        rtpbuf[7] = (udp_tim) & 0xFF;
        rtpbuf[8] = (rtpssrc >> 24) & 0xFF;
        rtpbuf[9] = (rtpssrc >> 16) & 0xFF;
        rtpbuf[10] = (rtpssrc >> 8) & 0xFF;
        rtpbuf[11] = (rtpssrc) & 0xFF;
        ++udp_seq;
        memcpy(rtpbuf + 12, buffer, s);
        if ((i = send_and_msg(socket_tcp, rtpbuf, (size_t) s + 12, MSG_DONTWAIT)) != s + 12) {
          if (i < 0)
            return;
        } else
          nBufferBytes -= s;
      } /* while(nBufferBytes) */
      i = recv(socket_tcp, rtpbuf, sizeof(rtpbuf), 0);
      if (i >= 12 && (unsigned char) rtpbuf[0] == (2 << 6)
          && rtpssrc
              == (unsigned int) (((unsigned char) rtpbuf[8] << 24)
                  + ((unsigned char) rtpbuf[9] << 16)
                  + ((unsigned char) rtpbuf[10] << 8)
                  + (unsigned char) rtpbuf[11])) {
        if (rtpbuf[1] == 96)
          rtptime = time(0);
        else if (rtpbuf[1] == 98) {
          flag_logical_error("Connection end\n");
          return;
        }
      } else if (time(0) > rtptime + 60) {
        flag_logical_error("Timeout\n");
        return;
      }
    }
    /*** Ntrip-Version 2.0 HTTP/1.1 ***/
    else if (nBufferBytes && (outmode == HTTP) && output_v2http_chunken) {
      if (!remainChunk) {
        int nChunkBytes = snprintf(szSendBuffer, sizeof(szSendBuffer), "%x\r\n", nBufferBytes);
        if (send_and_msg(sock, szSendBuffer, nChunkBytes, 0) < 0)
           return;
        remainChunk = nBufferBytes;
      }
      int i = send_and_msg(sock, buffer, (size_t) remainChunk, MSG_DONTWAIT);
      if (i < 0) {
        return;
      } else if (i) {
        memmove(buffer, buffer + i, (size_t) (nBufferBytes - i));
        nBufferBytes -= i;
        remainChunk -= i;
      } else {
        nBufferBytes = 0;
        remainChunk = 0;
      }
      if (!remainChunk) {
        if (send_and_msg(sock, "\r\n", strlen("\r\n"), 0) < 0)
          return;
      }
    }
    /*** Ntrip-Version 2.0 RTSP(TCP) / RTP(UDP) ***/
    else if ((nBufferBytes) && (outmode == RTSP)) {
      time_t ct;
      int r;
      char rtpbuffer[BUFSZ + 12];
      int i, j;
      gettimeofday(&now, NULL);
      /* RTP data packet generation*/
      if (isfirstpacket) {
        rtpseq = rand();
        rtptime = rand();
        last = now;
        isfirstpacket = 0;
      } else {
        ++rtpseq;
        sendtimediff = (((now.tv_sec - last.tv_sec) * 1000000)
            + (now.tv_usec - last.tv_usec));
        rtptime += sendtimediff / TIME_RESOLUTION;
      }
      rtpbuffer[0] = (RTP_VERSION << 6);
      /* padding, extension, csrc are empty */
      rtpbuffer[1] = 96;
      /* marker is empty */
      rtpbuffer[2] = rtpseq >> 8;
      rtpbuffer[3] = rtpseq;
      rtpbuffer[4] = rtptime >> 24;
      rtpbuffer[5] = rtptime >> 16;
      rtpbuffer[6] = rtptime >> 8;
      rtpbuffer[7] = rtptime;
      rtpbuffer[8] = rtpssrc >> 24;
      rtpbuffer[9] = rtpssrc >> 16;
      rtpbuffer[10] = rtpssrc >> 8;
      rtpbuffer[11] = rtpssrc;
      for (j = 0; j < nBufferBytes; j++) {
        rtpbuffer[12 + j] = buffer[j];
      }
      last.tv_sec = now.tv_sec;
      last.tv_usec = now.tv_usec;
      if ((i = sendto(sock, rtpbuffer, 12 + nBufferBytes, 0, pcasterRTP, length))
          != (nBufferBytes + 12)) {
        if (i < 0) {
          if (errno != EAGAIN) {
            flag_socket_error("WARNING: could not send data to Destination caster");
            return;
          }
        } else if (i) {
          memmove(buffer, buffer + (i - 12),
              (size_t) (nBufferBytes - (i - 12)));
          nBufferBytes -= i - 12;
        }
      } else {
        nBufferBytes = 0;
      }
      ct = time(0);
      if (ct - laststate > 15) {
        i = snprintf(buffer, sizeof(buffer),
            "GET_PARAMETER rtsp://%s%s/%s RTSP/1.0\r\n"
                "CSeq: %d\r\n"
                "Session: %u\r\n"
                "\r\n", casterouthost, rtsp_extension, mountpoint, udp_cseq++,
            rtpssrc);
        if (i > (int) sizeof(buffer) || i < 0) {
          flag_logical_error("Requested data too long");
          return;
        } else if (send_and_msg(socket_tcp, buffer, (size_t) i, 0) != i) {
          return;
        }
        laststate = ct;
      }
      /* ignore RTSP server replies */
      if ((r = recv(socket_tcp, buffer, sizeof(buffer), 0)) < 0) {
#ifdef WINDOWSVERSION
        if(WSAGetLastError() != WSAEWOULDBLOCK)
#else /* WINDOWSVERSION */
        if (errno != EAGAIN)
#endif /* WINDOWSVERSION */
        {
          flag_socket_error("Control connection closed\n");
          return;
        }
      } else if (!r) {
        flag_socket_error("Control connection read error\n");
        return;
      }
    }
    if (send_recv_success == 3)
      reconnect_sec = 1;
  } /* while (1) */
  return;
}

/********************************************************************
 * openserial
 *
 * Open the serial port with the given device name and configure it for
 * reading NMEA data from a GPS receiver.
 *
 * Parameters:
 *     tty     : pointer to    : A zero-terminated string containing the device
 *               unsigned char   name of the appropriate serial port.
 *     blocksz : integer       : Block size for port I/O  (ifndef WINDOWSVERSION)
 *     baud :    integer       : Baud rate for port I/O
 *
 * Return Value:
 *     The function returns a file descriptor for the opened port if successful.
 *     The function returns -1 / INVALID_HANDLE_VALUE in the event of an error.
 *
 * Remarks:
 *
 ********************************************************************/
#ifndef WINDOWSVERSION
static int openserial(const char *tty, int blocksz, int baud) {
  struct termios termios;

  /*** opening the serial port ***/
  gps_serial = open(tty, O_RDWR | O_NONBLOCK | O_EXLOCK);
  if (gps_serial < 0) {
    flag_io_error("ERROR: opening serial connection");
    return (-1);
  }

  /*** configuring the serial port ***/
  if (tcgetattr(gps_serial, &termios) < 0) {
    flag_io_error("ERROR: get serial attributes");
    return (-1);
  }
  termios.c_iflag = 0;
  termios.c_oflag = 0; /* (ONLRET) */
  termios.c_cflag = CS8 | CLOCAL | CREAD;
  termios.c_lflag = 0;
  {
    int cnt;
    for (cnt = 0; cnt < NCCS; cnt++)
      termios.c_cc[cnt] = -1;
  }
  termios.c_cc[VMIN] = blocksz;
  termios.c_cc[VTIME] = 2;

#if (B4800 != 4800)
  /* Not every system has speed settings equal to absolute speed value. */
  switch (baud) {
    case 300:
      baud = B300;
      break;
    case 1200:
      baud = B1200;
      break;
    case 2400:
      baud = B2400;
      break;
    case 4800:
      baud = B4800;
      break;
    case 9600:
      baud = B9600;
      break;
    case 19200:
      baud = B19200;
      break;
    case 38400:
      baud = B38400;
      break;
#ifdef B57600
    case 57600:
      baud = B57600;
      break;
#endif
#ifdef B115200
    case 115200:
      baud = B115200;
      break;
#endif
#ifdef B230400
    case 230400:
      baud = B230400;
      break;
#endif
#ifdef B921600
    case 921600:
      baud = B921600;
      break;
#endif
    default:
      flag_logical_error("WARNING: Baud settings not useful, using 19200\n");
      baud = B19200;
      break;
  }
#endif

  if (cfsetispeed(&termios, baud) != 0) {
    flag_io_error("ERROR: setting serial speed with cfsetispeed");
    return (-1);
  }
  if (cfsetospeed(&termios, baud) != 0) {
    flag_io_error("ERROR: setting serial speed with cfsetospeed");
    return (-1);
  }
  if (tcsetattr(gps_serial, TCSANOW, &termios) < 0) {
    flag_io_error("ERROR: setting serial attributes");
    return (-1);
  }
  if (fcntl(gps_serial, F_SETFL, 0) == -1) {
    flag_io_error("WARNING: setting blocking inputmode failed");
  }
  return (gps_serial);
}
#else
static HANDLE openserial(const char * tty, int baud) {
  char compath[15] = "";

  snprintf(compath, sizeof(compath), "\\\\.\\%s", tty);
  if((gps_serial = CreateFile(compath, GENERIC_WRITE|GENERIC_READ
  , 0, 0, OPEN_EXISTING, 0, 0)) == INVALID_HANDLE_VALUE)  {
    flag_io_error("ERROR: opening serial connection");
    return (INVALID_HANDLE_VALUE);
  }

  DCB dcb;
  memset(&dcb, 0, sizeof(dcb));
  char str[100];
  snprintf(str,sizeof(str),
  "baud=%d parity=N data=8 stop=1 xon=off octs=off rts=off",
  baud);

  COMMTIMEOUTS ct = {1000, 1, 0, 0, 0};

  if(!BuildCommDCB(str, &dcb))  {
    flag_io_error("ERROR: get serial attributes");
    return (INVALID_HANDLE_VALUE);
  }
  else if(!SetCommState(gps_serial, &dcb))  {
    flag_io_error("ERROR: set serial attributes\n");
    return (INVALID_HANDLE_VALUE);
  }
  else if(!SetCommTimeouts(gps_serial, &ct))  {
    flag_io_error("ERROR: set serial timeouts\n");
    return (INVALID_HANDLE_VALUE);
  }

  return (gps_serial);
}
#endif

/********************************************************************
 * usage
 *
 * Send a usage message to standard error and quit the program.
 *
 * Parameters:
 *     None.
 *
 * Return Value:
 *     The function does not return a value.
 *
 * Remarks:
 *
 *********************************************************************/
#ifdef __GNUC__
__attribute__ ((noreturn))
#endif /* __GNUC__ */
void usage(int rc, char *name) {
  fprintf(stderr, "Version %s (%s) GPL" COMPILEDATE "\nUsage:\n%s [OPTIONS]\n",
      revisionstr, datestr, name);
  fprintf(stderr, "PURPOSE\n");
  fprintf(stderr,
      "   The purpose of this program is to pick up a GNSS data stream (Input, Source)\n");
  fprintf(stderr, "   from either\n\n");
  fprintf(stderr, "     1. a Serial port, or\n");
  fprintf(stderr, "     2. an IP server, or\n");
  fprintf(stderr, "     3. a File, or\n");
  fprintf(stderr, "     4. a SISNeT Data Server, or\n");
  fprintf(stderr, "     5. a UDP server, or\n");
  fprintf(stderr, "     6. an NTRIP Version 1.0 Caster\n");
  fprintf(stderr, "     7. an NTRIP Version 2.0 Caster in HTTP mode \n\n");
  fprintf(stderr,
      "   and forward that incoming stream (Output, Destination) to either\n\n");
  fprintf(stderr, "     1. an NTRIP Version 2.0 Caster via TCP/IP (Output, Destination), or\n");
  fprintf(stderr, "     2. an NTRIP Version 2.0 Caster via RTSP/RTP (Output, Destination), or\n");
  fprintf(stderr, "     3. an NTRIP Version 2.0 Caster via plain UDP (Output, Destination), or\n");
  fprintf(stderr, "     4. an NTRIP Version 1.0 Caster, or\n");
  fprintf(stderr, "     5. an IP server via TCP/IP\n\n\n");
  fprintf(stderr, "OPTIONS\n");
  fprintf(stderr, "   -h|? print this help screen\n\n");
  fprintf(stderr, "    -E <ProxyHost>       Proxy server host name or address, required i.e. when\n");
  fprintf(stderr, "                         running the program in a proxy server protected LAN,\n");
  fprintf(stderr, "                         optional\n");
  fprintf(stderr, "    -F <ProxyPort>       Proxy server IP port, required i.e. when running\n");
  fprintf(stderr, "                         the program in a proxy server protected LAN, optional\n");
  fprintf(stderr, "    -R <maxDelay>        Reconnect mechanism with maximum delay between reconnect\n");
  fprintf(stderr, "                         attemts in seconds, default: no reconnect activated,\n");
  fprintf(stderr, "                         optional\n");
  fprintf(stderr, "    -L <FlagFile>        \"No connect\" flag file\n\n");
  fprintf(stderr, "    -M <InputMode> Sets the input mode (1 = Serial Port, 2 = IP server,\n");
  fprintf(stderr, "       3 = File, 4 = SISNeT Data Server, 5 = UDP server, 6 = NTRIP1 Caster,\n");
  fprintf(stderr, "       7 = NTRIP2 Caster in HTTP mode),\n");
  fprintf(stderr, "       mandatory\n\n");
  fprintf(stderr, "       <InputMode> = 1 (Serial Port):\n");
  fprintf(stderr, "       -i <Device>       Serial input device, default: %s, mandatory if\n", ttyport);
  fprintf(stderr, "                         <InputMode>=1\n");
  fprintf(stderr, "       -b <BaudRate>     Serial input baud rate, default: 19200 bps, mandatory\n");
  fprintf(stderr, "                         if <InputMode>=1\n");
  fprintf(stderr, "       -f <InitFile>     Name of initialization file to be send to input device,\n");
  fprintf(stderr, "                         optional\n\n");
  fprintf(stderr, "       <InputMode> = 2|5 (IP port | UDP port):\n");
  fprintf(stderr, "       -H <ServerHost>   Input host name or address, default: 127.0.0.1,\n");
  fprintf(stderr, "                         mandatory if <InputMode> = 2|5\n");
  fprintf(stderr, "       -P <ServerPort>   Input port, default: 1025, mandatory if <InputMode>= 2|5\n");
  fprintf(stderr, "       -f <ServerFile>   Name of initialization file to be send to server,\n");
  fprintf(stderr, "                         optional\n");
  fprintf(stderr, "       -x <ServerUser>   User ID to access incoming stream, optional\n");
  fprintf(stderr, "       -y <ServerPass>   Password, to access incoming stream, optional\n");
  fprintf(stderr, "       -B                Bind to incoming UDP stream, optional for <InputMode> = 5\n\n");
  fprintf(stderr, "       <InputMode> = 3 (File):\n");
  fprintf(stderr, "       -s <File>         File name to simulate stream by reading data from (log)\n");
  fprintf(stderr, "                         file, default is %s, mandatory for <InputMode> = 3\n\n",  filepath);
  fprintf(stderr, "       <InputMode> = 4 (SISNeT Data Server):\n");
  fprintf(stderr, "       -H <SisnetHost>   SISNeT Data Server name or address,\n");
  fprintf(stderr, "                         default: 131.176.49.142, mandatory if <InputMode> = 4\n");
  fprintf(stderr, "       -P <SisnetPort>   SISNeT Data Server port, default: 7777, mandatory if\n");
  fprintf(stderr, "                         <InputMode> = 4\n");
  fprintf(stderr, "       -u <SisnetUser>   SISNeT Data Server user ID, mandatory if <InputMode> = 4\n");
  fprintf(stderr, "       -l <SisnetPass>   SISNeT Data Server password, mandatory if <InputMode> = 4\n");
  fprintf(stderr, "       -V <SisnetVers>   SISNeT Data Server Version number, options are 2.1, 3.0\n");
  fprintf(stderr, "                         or 3.1, default: 3.1, mandatory if <InputMode> = 4\n\n");
  fprintf(stderr, "       <InputMode> = 6|7 (NTRIP Version 1.0|2.0 Caster):\n");
  fprintf(stderr, "       -H <SourceHost>   Source caster name or address, default: 127.0.0.1,\n");
  fprintf(stderr, "                         mandatory if <InputMode> = 6|7\n");
  fprintf(stderr, "       -P <SourcePort>   Source caster port, default: 2101, mandatory if\n");
  fprintf(stderr, "                         <InputMode> = 6|7\n");
  fprintf(stderr, "       -D <SourceMount>  Source caster mountpoint for stream input, mandatory if\n");
  fprintf(stderr, "                         <InputMode> = 6|7\n");
  fprintf(stderr, "       -U <SourceUser>   Source caster user Id for input stream access, mandatory\n");
  fprintf(stderr, "                         for protected streams if <InputMode> = 6|7\n");
  fprintf(stderr, "       -W <SourcePass>   Source caster password for input stream access, mandatory\n");
  fprintf(stderr, "                         for protected streams if <InputMode> = 6|7\n\n");
  fprintf(stderr, "    -O <OutputMode> Sets output mode for communication with destination caster / server\n");
  fprintf(stderr, "       1 = http  : NTRIP Version 2.0 Caster in TCP/IP mode\n");
  fprintf(stderr, "       2 = rtsp  : NTRIP Version 2.0 Caster in RTSP/RTP mode\n");
  fprintf(stderr, "       3 = ntrip1: NTRIP Version 1.0 Caster\n");
  fprintf(stderr, "       4 = udp   : NTRIP Version 2.0 Caster in Plain UDP mode\n");
  fprintf(stderr, "       5 = tcpip : IP server in TCP/IP mode\n\n\n");
  fprintf(stderr, "       Defaults to NTRIP1.0, but will change to 2.0 in future versions\n");
  fprintf(stderr, "       Note that the program automatically falls back from mode rtsp to mode http and\n");
  fprintf(stderr, "       further to mode ntrip1 if necessary.\n\n");
  fprintf(stderr, "       -a <DestHost>     Destination caster/server name or address, default: " NTRIP_CASTER ",\n");
  fprintf(stderr, "                         mandatory\n");
  fprintf(stderr, "       -p <DestPort>     Destination caster/server port, default: 2101,\n");
  fprintf(stderr, "                         mandatory\n");
  fprintf(stderr, "       -m <DestMount>    Destination caster mountpoint for stream upload,\n");
  fprintf(stderr, "                         only for NTRIP destination casters, mandatory\n");
  fprintf(stderr, "       -n <DestUser>     Destination caster user ID for stream upload to mountpoint,\n");
  fprintf(stderr, "                         only for NTRIP Version 2.0 destination casters, mandatory\n");
  fprintf(stderr, "       -c <DestPass>     Destination caster password for stream upload to mountpoint,\n");
  fprintf(stderr, "                         only for NTRIP destination casters, mandatory\n");
  fprintf(stderr, "       -N <STR-record>   Sourcetable STR-record\n");
  fprintf(stderr, "                         optional for NTRIP Version 2.0 in RTSP/RTP and TCP/IP mode\n\n");
  exit(rc);
} /* usage */

/********************************************************************/
/* signal handling                                                  */
/********************************************************************/
#ifdef __GNUC__
static void handle_sigint(int sig __attribute__((__unused__)))
#else /* __GNUC__ */
static void handle_sigint(int sig)
#endif /* __GNUC__ */
{
  sigint_received = 1;
  flag_logical_error("WARNING: SIGINT received - ntripserver terminates");
}

#ifndef WINDOWSVERSION
#ifdef __GNUC__
static void handle_alarm(int sig __attribute__((__unused__)))
#else /* __GNUC__ */
static void handle_alarm(int sig)
#endif /* __GNUC__ */
{
  if (!in_reconnect_pause) {
     sigalarm_received = 1;
     flag_int_error("ERROR: more than %d seconds no activity", ALARMTIME);
  };
}

#ifdef __GNUC__
static void handle_sigpipe(int sig __attribute__((__unused__)))
#else /* __GNUC__ */
static void handle_sigpipe(int sig)
#endif /* __GNUC__ */
{
  sigpipe_received = 1;
}
#endif /* WINDOWSVERSION */

static void setup_signal_handler(int sig, void (*handler)(int)) {
#if _POSIX_VERSION > 198800L
  struct sigaction action;

  action.sa_handler = handler;
  sigemptyset(&(action.sa_mask));
  sigaddset(&(action.sa_mask), sig);
  action.sa_flags = 0;
  sigaction(sig, &action, 0);
#else
  signal(sig, handler);
#endif
  return;
} /* setupsignal_handler */

/********************************************************************
 * base64-encoding                                                  *
 *******************************************************************/
static const char encodingTable[64] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
    'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/' };

/* does not buffer overrun, but breaks directly after an error */
/* returns the number of required bytes */
static int encode(char *buf, int size, const char *user, const char *pwd) {
  unsigned char inbuf[3];
  char *out = buf;
  int i, sep = 0, fill = 0, bytes = 0;

  while (*user || *pwd) {
    i = 0;
    while (i < 3 && *user)
      inbuf[i++] = *(user++);
    if (i < 3 && !sep) {
      inbuf[i++] = ':';
      ++sep;
    }
    while (i < 3 && *pwd)
      inbuf[i++] = *(pwd++);
    while (i < 3) {
      inbuf[i++] = 0;
      ++fill;
    }
    if (out - buf < size - 1)
      *(out++) = encodingTable[(inbuf[0] & 0xFC) >> 2];
    if (out - buf < size - 1)
      *(out++) = encodingTable[((inbuf[0] & 0x03) << 4)
          | ((inbuf[1] & 0xF0) >> 4)];
    if (out - buf < size - 1) {
      if (fill == 2)
        *(out++) = '=';
      else
        *(out++) = encodingTable[((inbuf[1] & 0x0F) << 2)
            | ((inbuf[2] & 0xC0) >> 6)];
    }
    if (out - buf < size - 1) {
      if (fill >= 1)
        *(out++) = '=';
      else
        *(out++) = encodingTable[inbuf[2] & 0x3F];
    }
    bytes += 4;
  }
  if (out - buf < size)
    *out = 0;
  return bytes;
}/* base64 Encoding */

/********************************************************************
 * send message to caster                                           *
 *********************************************************************/
static int send_to_caster(char *input, sockettype socket, int input_size) {
  int send_error = 1;

  if ((send(socket, input, (size_t) input_size, 0)) != input_size) {
    flag_socket_error("WARNING: could not send full header to Destination caster");
    send_error = 0;
  }
#ifndef NDEBUG
  else {
    printf("\nDestination caster request:\n");
    printf("%s\n", input);
  }
#endif
  return send_error;
}/* send_to_caster */

/********************************************************************
 * reconnect                                                        *
 *********************************************************************/
int reconnect(int rec_sec, int rec_sec_max) {
  if (rec_sec > rec_sec_max)
    rec_sec = rec_sec_max;
  printf("tcp connect pause %d sec\n", rec_sec);
  in_reconnect_pause = 1;
#ifndef WINDOWSVERSION
  alarm(0);
  sleep(rec_sec);
  sigpipe_received = 0;
  alarm(ALARMTIME);
#else
  Sleep(rec_sec*1000);
#endif
  rec_sec *= 2;
  in_reconnect_pause = 0;
  sigalarm_received = 0;
  return rec_sec;
} /* reconnect */
/********************************************************************
 * get current tick in ms                                           *
 *********************************************************************/
static uint32_t tickget(void)
{
#ifdef WINDOWSVERSION
    return (uint32_t)timeGetTime();
#else
    struct timespec tp={0};
    struct timeval  tv={0};

#ifdef CLOCK_MONOTONIC_RAW
    /* linux kernel > 2.6.28 */
    if (!clock_gettime(CLOCK_MONOTONIC_RAW,&tp)) {
        return tp.tv_sec*1000u+tp.tv_nsec/1000000u;
    }
    else {
        gettimeofday(&tv,NULL);
        return tv.tv_sec*1000u+tv.tv_usec/1000u;
    }
#else
    gettimeofday(&tv,NULL);
    return tv.tv_sec*1000u+tv.tv_usec/1000u;
#endif
#endif /* WINDOWSVERSION */
}
/********************************************************************
 * copy str in printable forn                                       *
 *********************************************************************/
int str_as_printable(const char *szSendBuffer, int nBufferBytes, char *msgbuf, size_t msgbufSize)
{
  int msglen = 0;
  const char *a;
  for (a = szSendBuffer; *a; ++a) {
     if (isprint(*a))
        msgbuf[msglen++] = *a;
     else if ((*a == '\n') || (*a == '\r'))
        msglen += snprintf(msgbuf+msglen, msgbufSize-msglen, "\\%c", (*a == '\n') ? 'n' : 'r');
     else
        msglen += snprintf(msgbuf+msglen, msgbufSize-msglen, "\\x%02X", *a);
  }
  msglen += snprintf(msgbuf+msglen, msgbufSize-msglen, "(len=%d)", nBufferBytes);
  return msglen;
}
/********************************************************************
 * set reconnect time at hard error                                 *
 *********************************************************************/
void set_reconnect_time_at_hard_error(void)
{
  if (was_1st_ntrip_connect) {
    if (reconnect_sec < MIN_PAUSE_AT_UNAUTH)
      reconnect_sec = MIN_PAUSE_AT_UNAUTH;
  } else
    reconnect_sec = reconnect_sec_max;
}
/********************************************************************
 * receive all from custer                                          *
 *********************************************************************/
int recv_from_caster(char *szSendBuffer, size_t bufferSize, char *msgbuf, size_t msgbufSize, const char *protocolName)
{
    int nBufferBytes = 0;
    *szSendBuffer = '\0';
    uint32_t startTick = tickget();
    *msgbuf = 0;
    /* check Destination caster's response */
    while (nBufferBytes < (int)bufferSize) {
      int nread = recv(socket_tcp, &szSendBuffer[nBufferBytes],  bufferSize - nBufferBytes, 0);
      if (nread <= 0) {
        int err=errsock();
        if ((err!=EALREADY) && (err!=EINPROGRESS)) {
          if (err==0)  {
            if ((nBufferBytes>=NTRIP_MINRSP) && strstr(szSendBuffer,"\n\r\n"))
               break;
            if (nBufferBytes) {
               int msglen = snprintf(msgbuf, msgbufSize, "%s connection recv disconnected by %s:%d Response: ",
                                     protocolName, casterouthost, casteroutport);
               str_as_printable(szSendBuffer, nBufferBytes, msgbuf+msglen, msgbufSize-msglen);
            } else {
               snprintf(msgbuf, msgbufSize, "%s connection recv disconnected by %s:%d with EMPTY response",
                        protocolName, casterouthost, casteroutport);
               if (reconnect_sec < MIN_PAUSE_AT_EMPTY)
                  reconnect_sec = MIN_PAUSE_AT_EMPTY;
            }
          } else
            snprintf(msgbuf, msgbufSize, "%s connection recv error %d (%s) at %s:%d",
                     protocolName, err, errorstring(err), casterouthost, casteroutport);
          break;
        } else {
          usleep(1000L);
          continue;
        }
      } // if (nread <= 0)
      nBufferBytes += nread;
      szSendBuffer[nBufferBytes] = '\0';
      if ((nBufferBytes>=NTRIP_MINRSP) && strstr(szSendBuffer,"\n\r\n"))
        break;
      if ((int)(tickget()-startTick)>=MAX_NTRIP_CONNECT_TIME) {
        if (nBufferBytes)
          snprintf(msgbuf, msgbufSize, "%s connection timeout from %s:%d (server answer: %.*s)",
                   protocolName, casterouthost, casteroutport, nBufferBytes-2, szSendBuffer);
        else
          snprintf(msgbuf, msgbufSize, "%s connection no answer from %s:%d",
                   protocolName, casterouthost, casteroutport);
        break;
      }
    } // while (output_init && (nBufferBytes < (int) sizeof(szSendBuffer))
    return nBufferBytes;
}
/********************************************************************
 * flag create and erase                                            *
 *********************************************************************/
static void flag_create(const char *msg)
{
  if (!*flagpath) return;
  FILE *file=fopen(flagpath,"wt");
  if (file) {
     fputs(msg,file);
     fclose(file);
  }
}
/* erase error flag ----------------------------------------------------------*/
static void flag_erase(void)
{
  if (!*flagpath) return;
  remove(flagpath);
}
/********************************************************************
 * error operation                                                  *
 *********************************************************************/
/* get socket error ----------------------------------------------------------*/
#ifdef WINDOWSVERSION
static int errsock(void) {return WSAGetLastError();}
#else
static int errsock(void) {return errno;}
#endif
/* get error text ------------------------------------------------------------*/
static char errbuf[200];
static char *errorstring(int err)
{
    strncpy(errbuf,strerror(err),sizeof(errbuf));
    errbuf[sizeof(errbuf)-1] = 0;
    int Len = (int)strlen(errbuf)-1;
    while ((Len >= 0) && (errbuf[Len] < ' ')) --Len;
    errbuf[Len+1] = 0;
    return errbuf;
}
static void flag_logical_error(const char *msg)
{
  flag_create(msg);
  fprintf(stderr,"%s\n",msg);
}
static void flag_error(const char *msg, int err)
{
  char buf[FLAG_MSG_SIZE];
  snprintf(buf, sizeof(buf), "%s %d (%s)", msg, err, errorstring(err));
  buf[sizeof(buf)-1] = 0;
  flag_logical_error(buf);
}
static void flag_socket_error(const char *msg)
{
  flag_error(msg, errsock());
}
static void flag_io_error(const char *msg)
{
  flag_error(msg, errno);
}
static void flag_str_error(const char *msg, const char *arg)
{
  char buf[FLAG_MSG_SIZE];
  snprintf(buf, sizeof(buf), msg, arg);
  buf[sizeof(buf)-1] = 0;
  flag_logical_error(buf);
}
static void flag_int_error(const char *msg, int value)
{
  char buf[FLAG_MSG_SIZE];
  snprintf(buf, sizeof(buf), msg, value);
  buf[sizeof(buf)-1] = 0;
  flag_logical_error(buf);
}
/********************************************************************
 * close session                                                    *
 *********************************************************************/
static void close_session(const char *caster_addr, const char *mountpoint,
    int session, char *rtsp_ext, int fallback) {
  int size_send_buf;
  char send_buf[BUFSZ];

  if (!fallback) {
    if ((gps_socket != INVALID_SOCKET)
        && ((inputmode == TCPSOCKET) || (inputmode == UDPSOCKET)
            || (inputmode == NTRIP1_IN) || (inputmode == NTRIP2_HTTP_IN)
            || (inputmode == SISNET))) {
      if (closesocket(gps_socket) == -1) {
        flag_socket_error("ERROR: close input device ");
        exit(IO_ERROR);
      } else {
        gps_socket = -1;
#ifndef NDEBUG
        printf("close input device: successful\n");
#endif
      }
    } else if ((gps_serial != INVALID_HANDLE_VALUE) && (inputmode == SERIAL)) {
#ifndef WINDOWSVERSION
      if (close(gps_serial) == INVALID_HANDLE_VALUE) {
        flag_io_error("ERROR: close input device ");
        exit(IO_ERROR);
      }
#else
      if(!CloseHandle(gps_serial))
      {
        flag_io_error("ERROR: close input device ");
        exit(IO_ERROR);
      }
#endif
      else {
        gps_serial = INVALID_HANDLE_VALUE;
#ifndef NDEBUG
        printf("close input device: successful\n");
#endif
      }
    } else if ((gps_file != -1) && (inputmode == INFILE)) {
      if (close(gps_file) == -1) {
        flag_io_error("ERROR: close input device ");
        exit(IO_ERROR);
      } else {
        gps_file = -1;
#ifndef NDEBUG
        printf("close input device: successful\n");
#endif
      }
    }
  }

  if (socket_udp != INVALID_SOCKET) {
    if (udp_cseq > 2) {
      size_send_buf = snprintf(send_buf, sizeof(send_buf),
          "TEARDOWN rtsp://%s%s/%s RTSP/1.0\r\n"
              "CSeq: %d\r\n"
              "Session: %u\r\n"
              "\r\n", caster_addr, rtsp_ext, mountpoint, udp_cseq++, session);
      if ((size_send_buf >= (int) sizeof(send_buf)) || (size_send_buf < 0)) {
        flag_logical_error("ERROR: Destination caster request to long\n");
        exit(IO_ERROR);
      }
      send_to_caster(send_buf, socket_tcp, size_send_buf);
      strcpy(send_buf, "");
      size_send_buf = recv(socket_tcp, send_buf, sizeof(send_buf), 0);
      send_buf[size_send_buf] = '\0';
#ifndef NDEBUG
      printf("Destination caster response:\n%s", send_buf);
#endif
    }
    if (closesocket(socket_udp) == -1) {
      flag_socket_error("ERROR: close udp socket");
      exit(IO_ERROR);
    } else {
      socket_udp = -1;
#ifndef NDEBUG
      printf("close udp socket: successful\n");
#endif
    }
  }

  if (socket_tcp != INVALID_SOCKET) {
    if (closesocket(socket_tcp) == -1) {
      flag_socket_error("ERROR: close tcp socket");
      exit(IO_ERROR);
    } else {
      socket_tcp = -1;
#ifndef NDEBUG
      printf("close tcp socket: successful\n");
#endif
    }
  }
} /* close_session */
