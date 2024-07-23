#include <zlib.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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
 * - select(2), and (gz)read data when available
 * - Exit when <exit/> is received
 */

int
servermain(int argc, char** argv)
{
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *rp;
    int listenfd;
    int sfd;
    int s;
    struct sockaddr_storage peer_addr;
    socklen_t peerlen = sizeof peer_addr;
    ssize_t nread;
    char buf[BUF_SIZE];
    gzFile gzfi = NULL;

    if (argc != 2) {
        fprintf(stderr, "Server usage: %s port\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Setup for getaddrinfo(3) call */

    memset(&hints, 0, sizeof(hints));
    //hints.ai_family = AF_UNSPEC;       /* Allow IPv4 or IPv6 */
    //hints.ai_family = AF_INET6;                      /* IPv6 */
    hints.ai_family = AF_INET;                         /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;        /* Datagram socket */
    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
    hints.ai_protocol = 0;                     /* Any protocol */
    hints.ai_canonname = NULL;   /* Server; nobbut else needed */
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    /* Get address info:  NULL => server; argv[1] is the port */
    s = getaddrinfo(NULL, argv[1], &hints, &result);
    if (s != 0) {
        fprintf(stderr, "Server getaddrinfo failed: %s\n", gai_strerror(s));
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
    for (int zs = 0;; ++zs)
    {
        int rtn;
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

            continue;  /* Done with gzread over socket; skip listenfd */

        } /* if (gzfi) */

        /* To here, there is no active accepted socket; select(2) result
         * was for bound listining socket, indicating a new connection
         * request
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
        s = getnameinfo((struct sockaddr *)&peer_addr, peerlen
                       , host, NI_MAXHOST
                       , service, NI_MAXSERV, NI_NUMERICSERV);
        if (s == 0)
        {
            fprintf(stderr, "Server accepted connection from %s:%s\n", host, service);
        }
        else
        {
            fprintf(stderr, "Server getnameinfo failed: %s\n", gai_strerror(s));
        }

        /* Open/allocate the gzFile, and set its buffer size */
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
    } // for (int zs = 0;; ++zs)

    return EXIT_SUCCESS;
}

int
clientmain(int argc, char** argv)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sfd, s;
    size_t len;
    ssize_t nread;
    char buf[BUF_SIZE];
    gzFile gzfi = NULL;
    int final_rtn = EXIT_SUCCESS;

    if (argc < 3) {
        fprintf(stderr, "Client usage: %s host port msg...\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Obtain address(es) matching host/port */

    memset(&hints, 0, sizeof(hints));
    //hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    //hints.ai_family = AF_INET6;      /* Allow IPv4 or IPv6 */
    hints.ai_family = AF_INET;      /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* Datagram socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    s = getaddrinfo(argv[1], argv[2], &hints, &result);
    if (s != 0) {
        fprintf(stderr, "Client getaddrinfo failed: %s\n", gai_strerror(s));
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

    if (!(gzfi = gzdopen(sfd, "w")))
    {
        fprintf(stderr, "Client gzdopen(%d, \"w\") failed\n" , sfd);
        close(sfd);
        sfd = -1;
        return EXIT_FAILURE;
    }

    /* Send remaining command-line arguments as separate writes */

    for (int j = 3; final_rtn == EXIT_SUCCESS && j < argc; j++)
    {
        int rtn;
        int itmp;
        int save_errno;

        len = strlen(argv[j]) + 1;    /* +1 for terminating null byte */

        if (len > BUF_SIZE)
        {
            fprintf(stderr,  "Client Ignoring long message"
                             " in argument %d\n", j
                   );
            continue;
        }

        if (!strcmp(argv[j],"--delay"))
        {
            struct timeval tv;
            tv = tvfixed;
            select(0,NULL,NULL,NULL,&tv);
            continue;
        }

        errno = 0;
        if ((rtn=gzwrite(gzfi, argv[j], len)) != len)
        {
            save_errno = errno;
            fprintf(stderr, "Client partial/failed %d=gzwrite[%s]"
                            "; %d=errno[%s]"
                          , rtn, gzerror(gzfi, &itmp)
                          , save_errno, strerror(save_errno)
                   );
            final_rtn = EXIT_FAILURE;
            continue;
        }

        errno = 0;
        if ((rtn=gzflush(gzfi, Z_SYNC_FLUSH)) != Z_OK)
        {
            save_errno = errno;
            fprintf(stderr, "Client partial/failed %d=gzbuffer[%s]"
                            "; %d=errno[%s]"
                          , rtn, gzerror(gzfi, &itmp)
                          , save_errno, strerror(save_errno)
                   );
            final_rtn = EXIT_FAILURE;
            continue;
        }
    } // for (int j = 3; j < argc; j++)

    gzflush(gzfi, Z_FINISH);
    //gzclose(gzfi);

    return final_rtn;
}

int
main(int argc, char** argv)
{
  if (argc < 3) { return servermain(argc, argv); }
  return clientmain(argc, argv);
}
