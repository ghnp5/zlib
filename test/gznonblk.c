/* gznonblk.c -- test non-blocking reads w/zlib compression library
 * Copyright (C) 2024 Brian T. Carcich
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include <zlib.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#define BUF_SIZE 128
const struct timeval tvfixed = { 1, 0 };

/* Wrapper to make file non-blocking */

int
make_fd_nonblocking(int fd)
{
    int existing_flags = fcntl(fd, F_GETFL);
    return (-1 == existing_flags)
           ? -2
           : fcntl(fd, F_SETFL, existing_flags | O_NONBLOCK);
}

/* Server code to use gzread.c/zlib.h library:
 * - Listen for, and accept, socket connection(s)
 * - Make connection non-blocking
 * - select(2), and (gz)read data when available
 * - Exit when "-stopserver-" is received
 */

int
servermain(int argc, char** argv)
{
    struct addrinfo *rp;                      /* getaddrinfo(3) items */
    struct addrinfo hints;
    struct addrinfo *result;
    struct sockaddr_storage peer_addr;
    socklen_t peerlen = sizeof peer_addr;

    int rtn;                       /* Return value from many routines */
    int sfd;     /* Accepted socket file descriptor, for sending data */
    int listenfd;      /* Bound socket file descriptor, for listening */
    int stopserver = 0;          /* Flag for when to exit server code */
    char buf[BUF_SIZE];                       /* Read (gzread) buffer */
    gzFile gzfi = NULL;                  /* gzread "file" information */

    if (argc != 2) {
        fprintf(stderr, "Server usage: %s port|service\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Setup for getaddrinfo(3) call */

    memset(&hints, 0, sizeof(hints));
    //hints.ai_family = AF_UNSPEC;              /* Allow IPv4 or IPv6 */
    //hints.ai_family = AF_INET6;                             /* IPv6 */
    hints.ai_family = AF_INET;                                /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;                 /* Stream socket */
    hints.ai_flags = AI_PASSIVE;           /* For wildcard IP address */
    hints.ai_protocol = 0;                            /* Any protocol */
    hints.ai_canonname = NULL;          /* Server; nobbut else needed */
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    /* Get address info:  NULL => server; argv[1] is the port */
    rtn = getaddrinfo(NULL, argv[1], &hints, &result);
    if (rtn) {
        fprintf(stderr, "Server %d=getaddrinfo(\"%s\",\"%s\",...)"
                        " failed: %s\n"
                      , rtn, "<null>", argv[1], gai_strerror(rtn)
               );
        return EXIT_FAILURE;
    }

    /* getaddrinfo() returns a list of address structures.
     * Try each address structure until we successfully bind(2).
     * If socket(2) or bind(2) or listen(2) fails, then we try the next
     * address structure after closing the socket if socket(2) succeeded
     */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listenfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listenfd == -1) { continue; }

        if (!bind(listenfd, rp->ai_addr, rp->ai_addrlen)
         && !listen(listenfd, 10)
           )
        {
            break;                  /* Success */
        }
        close(listenfd);
    }

    freeaddrinfo(result);                         /* No longer needed */

    if (rp == NULL) {                         /* No address succeeded */
        fprintf(stderr, "Server could not socket/bind/listen\n");
        return EXIT_FAILURE;
    }

    sfd=-1;
    /* Loop:
     * - select(2) on EITHER listening OR connected socket
     *   - accept a new socket connection if listening socket has data
     *   - select(2)/gzread data from connected socket, print to stderr
     * - Close when socket is closed
     *
     * zs is the number of consecutive times select(2) returns 0
     */
    for (int zs = 0; !stopserver; ++zs)
    {
        int nfd;
        fd_set rfds;
        struct timeval tv;
        char host[NI_MAXHOST];
        char service[NI_MAXSERV];

        /* Clear FD set */
        FD_ZERO(&rfds);

        if (gzfi)
        {
            /* Set accepted socket's bit if gzfi is not NULL */
            FD_SET(sfd, &rfds);
            nfd = sfd + 1;
        }
        else
        {
            /* Set listening socket's bit if gzfi is NULL */
            FD_SET(listenfd, &rfds);
            nfd = listenfd + 1;
        }

        /* Wait for data from selected file */
        errno = 0;
        tv = tvfixed;
        rtn = select(nfd, &rfds, NULL, NULL, &tv);
        if (rtn)
        {
            /* Log non-zero return values from select(2) */
            fprintf(stderr, "Server %d=select(nfd,%llx,,,tv)"
                            "; errno=%d[%s]\n"
                          , rtn, *((long long*)&rfds)
                          , errno, strerror(errno)
                   );
            zs = 0;
        }
        else
        {
            /* Count and log consecutive passes with no incoming data */
            const char terms[] = { "|/-\\" };
            fprintf(stderr, "%d%c\r", zs, terms[zs&3]);
            continue;
        }

        if (rtn < 0)                                 /* select failed */
        {
            select(0, NULL, NULL, NULL, &tv);         /* finish delay */
            continue;                         /* Ignore failed select */
        }

        /* to here, select(2) returned 1 */

        if (gzfi)         /* if accepted socket is active, handle I/O */
        {
            char* p;
            char* pend;
            int ipos;

            //not necessary:  if (!FD_ISSET(sfd, &rfds)) continue;

            /* Read data */
            errno = 0;
            gzclearerr(gzfi);
            rtn = gzread(gzfi, buf, sizeof buf);
            fprintf(stderr, "Server %d=gzread(%d,...)"
                            "; errno=%d[%s]\n"
                          , rtn, sfd, errno, strerror(errno)
                   );

            /* Handle EOF (rtn==0) or error (rtn<0) */
            if (rtn < 1)
            {
                int igzerr;
                const char* pgzerr = gzerror(gzfi, &igzerr);
                fprintf(stderr, "Server %d=gzerror[%s]\n"
                              , igzerr, pgzerr ? pgzerr : "<null>"
                       );
                gzclose(gzfi);
                gzfi = NULL;
                sfd = -1;
                continue;
            }

            /* Log data read by gzread */
            fprintf(stderr, "%s", "Server buf=>[");
            for (pend = (p=buf) + rtn; p<pend; ++p)
            {
              fprintf(stderr, (32<=*p&&*p<127) ? "%c" : "<0x%02x>", *p);
            }
            fprintf(stderr, "%s", "]\n");

            /* If "-stopserver-" was received, then set flag */
            stopserver = (rtn == 12 || (rtn == 13 && !buf[12]))
                      && !strncmp("-stopserver-", buf, 12);

            continue;  /* Done with gzread over socket; skip listenfd */
        } /* if (gzfi) */

        /* To here, there is no active accepted socket; select(2) result
         * was for bound listening socket (listenfd), indicating a new
         * connection request
         */

        //not necessary:  if (!FD_ISSET(listenfd, &rfds)) { continue; }

        /* Accept the new connection */
        errno = 0;
        sfd = accept(listenfd, (struct sockaddr *)&peer_addr, &peerlen);
        if (sfd < 0)
        {
            fprintf(stderr, "Server %d=accept(listenfd,...)"
                            "; errno=%d[%s]\n"
                          , rtn, errno, strerror(errno)
                   );
            continue;
        }

        /* Make the new socket non-blocking */
        errno = 0;
        if (0 > make_fd_nonblocking(sfd))
        {
            fprintf(stderr, "Server %d=make_fd_nonblocking(%d,...)"
                            "; errno=%d[%s]\n"
                          , rtn, sfd, errno, strerror(errno)
                   );
            close(sfd);
            sfd = -1;
            continue;
        }

        /* Log information about the newly-accepted socket */
        rtn = getnameinfo((struct sockaddr *)&peer_addr, peerlen
                         , host, NI_MAXHOST
                         , service, NI_MAXSERV, NI_NUMERICSERV);
        if (rtn == 0)
        {
            fprintf(stderr, "Server accepted connection from %s:%s\n"
                          , host, service
                   );
        }
        else
        {
            fprintf(stderr, "Server getnameinfo failed: %s\n"
                          , gai_strerror(rtn)
                   );
        }

        /* Allocate/open the gzFile pointer, and set its buffer size */
        if (!(gzfi = gzdopen(sfd, "r")))
        {
            fprintf(stderr, "Server gzdopen(%d, \"r\") failed\n" , sfd);
            close(sfd);
            sfd = -1;
            continue;
        }
        if (gzbuffer(gzfi, 16))
        {
            fprintf(stderr, "%s\n", "Server gzbuffer(gzfi, 16) failed");
            gzclose(gzfi);
            gzfi = NULL;
            sfd = -1;
            continue;
        }
    } // for (int zs = 0; !stopserver; ++zs)

    return EXIT_SUCCESS;
}

/* Client code to use gzwrite.c/zlib.h library:
 * - Open socket connection to server above
 * - Make connection non-blocking
 * - gzwrite command-line arguments (argv)
 * - Exit after last argument has been "gzwritten"
 */

int
clientmain(int argc, char** argv)
{
    struct addrinfo *rp;                      /* getaddrinfo(3) items */
    struct addrinfo hints;
    struct addrinfo *result;

    int rtn;                       /* Return value from many routines */
    int sfd;                                /* Socket file descriptor */
    size_t len;                             /* Length of data to send */
    char* serverhost;           /* Name of server (argv[2] or argv[3] */
    gzFile gzfi = NULL;                  /* gzread "file" information */
    int final_rtn = EXIT_SUCCESS;        /* Exit code; assume success */
    int clientfork = argc > 2 && !strcmp(argv[2], "--client-fork");

    if (argc < 3 || (argc == 3 && clientfork))
    {
        fprintf(stderr, "Client usage: %s port|service%s"
                        " serverhost msg...\n"
                      , argv[0], clientfork ? " --client-fork" : ""
               );
        return EXIT_FAILURE;
    }

    /* Extract server hostname apropo the command line:
     *
     *     gznonblk portnumber serverhost ...
     *
     * OR
     *
     *     gznonblk portnumber --client-fork serverhost ...
     */
    serverhost = clientfork ? argv[3] : argv[2];

    /* Obtain address(es) matching host/port */

    memset(&hints, 0, sizeof(hints));
    //hints.ai_family = AF_UNSPEC;              /* Allow IPv4 or IPv6 */
    //hints.ai_family = AF_INET6;                  /* Allow IPv6 only */
    hints.ai_family = AF_INET;                     /* Allow IPv4 only */
    hints.ai_socktype = SOCK_STREAM;                 /* Stream socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;                            /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    /* Get server address info:  argv[1] is the port */
    errno = 0;
    rtn = getaddrinfo(serverhost, argv[1], &hints, &result);
    if (rtn) {
        fprintf(stderr, "Client %d=getaddrinfo(\"%s\",\"%s\",...)"
                        " failed: %s\n"
                      , rtn, serverhost, argv[1], gai_strerror(rtn)
               );
        return EXIT_FAILURE;
    }

    /* getaddrinfo() returns a list of address structures.
       Try each address until we successfully connect(2).
       If socket(2) (or connect(2)) fails, we (close the socket
       and) try the next address. */

    for (rp = result; rp != NULL; rp = rp->ai_next) {

        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sfd == -1) { continue; }  /* Ignore failed socket creation*/

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) { break; }
        /* ^ Exit loop on Successful connection              */
        /* v Close socket and continue on Failed connection  */
        close(sfd);
    }

    freeaddrinfo(result);  /* getaddrinfo result is no longer needed */

    if (rp == NULL)                   /* No address means all failed */
    {
        fprintf(stderr, "Client could not connect\n");
        return EXIT_FAILURE;
    }

    /* Allocate/open the gzFile pointer */
    if (!(gzfi = gzdopen(sfd, "w")))
    {
        fprintf(stderr, "Client gzdopen(%d, \"w\") failed\n" , sfd);
        close(sfd);
        sfd = -1;
        return EXIT_FAILURE;
    }

    /* Send remaining command-line arguments as separate gzwrites */

    for (int iarg = clientfork ? 4 : 3
        ; final_rtn == EXIT_SUCCESS && iarg < argc
        ; ++iarg
        )
    {
        int itmp;                        /* Unused value from gzerror */
        int save_err;                              /* cache for errno */

        if (!strcmp(argv[iarg],"--delay"))   /* argument is "--delay" */
        {
            struct timeval tv;
            tv = tvfixed;
            select(0,NULL,NULL,NULL,&tv); /* Delay, then get next arg */
            continue;
        }

        len = strlen(argv[iarg]) + 1; /* +1 for terminating null byte */

        /* Write argument using gzwrite; on error log and exit */
        errno = 0;
        if ((rtn=gzwrite(gzfi, argv[iarg], len)) != len)
        {
            save_err = errno;
            fprintf(stderr, "Client partial/failed %d=gzwrite[%s]"
                            "; %d=errno[%s]"
                          , rtn, gzerror(gzfi, &itmp)
                          , save_err, strerror(save_err)
                   );
            final_rtn = EXIT_FAILURE;
            continue;
        }

        /* Flush data to socket */
        errno = 0;
        if ((rtn=gzflush(gzfi, Z_SYNC_FLUSH)) != Z_OK)
        {
            save_err = errno;
            fprintf(stderr, "Client partial/failed %d=gzbuffer[%s]"
                            "; %d=errno[%s]"
                          , rtn, gzerror(gzfi, &itmp)
                          , save_err, strerror(save_err)
                   );
            final_rtn = EXIT_FAILURE;
            continue;
        }

      /* End of argument loop */
    } // for (...; final_return == EXIT_SUCCESS ...; ++iarg)

    /* Execute final flush, close socket, return status */
    gzflush(gzfi, Z_FINISH);
    gzclose(gzfi);

    return final_rtn;
}

int Usage(int argc, char** argv)
{
    const char* arrUsage[] =
    { "Usage:"
    , "  gznonblk pn[[ --client-fork] srvrhost[ msg1|--delay[ msg2...]]]"
    , "  gznonblk --help[-long]"
    , ""
    , "where"
    , "            pn = port# or service where server will be listening"
    , " --client-fork = directive to run server and fork client"
    , "      srvrhost = hostname of server for client to use"
    , "  msgN|--delay = client messages to send or delays between them"
    , NULL
    , ""
    , "Examples:"
    , ""
    , "  gznonblk 4444"
    , "  - Start server only, listening on port 4444"
    , ""
    , "  gznonblk 4444 srvrhost message1 --delay message2 message3"
    , "  - Start client only, connect to server at port 4444 on srvrhost"
    , "    - Client"
    , "      - sends \"message1\""
    , "      - delays"
    , "      - sends \"message2\" and \"message3\""
    , ""
    , "  gznonblk 4444 --client-fork 127.0.0.1 msg1 --delay -stopserver-"
    , "  - Fork client, connect to server at port 4444 on 127.0.0.1"
    , "    - Client"
    , "      - delays for server to start (forced when forking client)"
    , "      - sends \"msg1\""
    , "      - delays"
    , "      - sends \"-stopserver-\""
    , "        - which will stop server later"
    , "  - Start server, listening on port 4444"
    , NULL
    };
    int helplong = 0;

    while (--argc)
    {
        if (!strcmp(argv[argc], "--help")) { break; }
        if (!strcmp(argv[argc], "--help-long")) { ++helplong; break; }
    }
    if (!argc) { return 0; }

    for (char** p=(char**)arrUsage; helplong || *p; ++p)
    {
        if (!*p) { --helplong; continue; }
        fprintf(stdout, "%s\n", *p);
    }
    return 1;
}

int
main(int argc, char** argv)
{
    /* Check if "--client-fork" is at argument offset 2 */
    int clientfork = argc > 2 && !strcmp(argv[2], "--client-fork");

    if (Usage(argc, argv)) { return EXIT_SUCCESS; }

    /* If command line is:
     *
     *     gznonblk portnum serverhost ...
     *
     * then run client only
     */
    if (argc > 2 && !clientfork) { return clientmain(argc, argv); }

    /* If command line is:
     *
     *     gznonblk portnum --clienthost serverhost ...
     *
     * then fork client, run server, wait for client
     */
    errno = 0;
    if (clientfork)
    {
        int rtn;                   /* Return value from many routines */
        int wstat;            /* Client return status from waitpid(2) */
        pid_t pidwaited;              /* Return value from waitpid(2) */
        int badchild = 0;     /* Non-zero flag for any client failure */
        int wopts = WNOHANG;                /* Options for waitpid(2) */
        struct timeval tv = {3, 0};    /* Delay 3s for client to exit */

        pid_t pidforked = fork();                  /* Fork the client */

        int save_err = errno;              /* Save the fork(2) result */

        /* On fork(2) error, log and exit */
        if (pidforked < 0)
        {
            fprintf(stderr, "Server %d=fork() of client failed"
                            "; %d=errno[%s]\n"
                          , pidforked, save_err, strerror(save_err)
                   );
            return EXIT_FAILURE;
        }

        /* If this is now forked child, then run client and exit */
        if (!pidforked) { return clientmain(argc, argv); }

        /* To here, this is the server */
        fprintf(stderr, "Server %d=fork()=PID of client succeeded"
                        "; %d=errno[%s]\n"
                      , pidforked, save_err, strerror(save_err)
               );

        rtn = servermain(2, argv);                      /* Run server */
        select(0, NULL, NULL, NULL, &tv); /* Delay for client to exit */
        errno = 0;
        pidwaited = waitpid(-1, &wstat, wopts);  /* Get client status */

        if (pidwaited < 1)        /* Non-positive return is a failure */
        {
            ++badchild;
            fprintf(stderr, "Server child %d=waitpid(-1,...) failed"
                            "; %d=errno[%s]\n"
                          , pidwaited, errno, strerror(errno)
                   );
            return EXIT_FAILURE;
        }

        if (pidwaited != pidforked)   /* Wait PID must match fork PID */
        {
            ++badchild;
            fprintf(stderr, "Server child %d=waitpid(-1,...) not equal"
                            " to forked pid (%d)\n"
                          , pidwaited, pidforked
                   );
            return EXIT_FAILURE;
        }

        /* Combine server status and child/client status */
        return rtn | (WIFEXITED(wstat) ? WEXITSTATUS(wstat) : 1);
    }

    /* If command line is:
     *
     *     gznonblk portnum
     *
     * then run server only
     */
    if (argc == 2) { return servermain(2, argv); }

    return -1;
}
