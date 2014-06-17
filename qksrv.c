/*
 * look at minihttp/microhttp code for ideas on code arragement
 * mime types
 * * Fix request->resource allocations and deallocation
 * * split up request_process
 * make a macro out of request_destroy
 * pthread/select/event loop
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>

#define BACKLOG 1000 // Pending connections
#define PORT "8888"
#define MAXHEADERSIZE 8192
#define QKSRV_HTTPVERSION "HTTP/1.0"
#define QKSRV_NAME "qksrv"
#define DATETIMEFORMAT "%a, %d %b %y %T %Z"

typedef enum {
    GET,
    HEAD,
    POST,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATCH,
    UNSUPPORTED
} Method;

typedef enum {
    HTTP10,
    HTTP11
} Httpver;

typedef struct {
    int sockfd;
    // Where the request came from
    Method method;
    // Which method the request requested
    Httpver http_version;
    char *resource;
    // dont care about the rest of the header
} Request;

int sendall_buffer(int sock, char *buf, int len) {

    int total = 0;
    int bytesleft = len;
    int n;

    while (total < len) {
        n = send(sock, buf+total, bytesleft, 0);
        if (n == -1) {
            break;
        }
        total = total + n;
        bytesleft = bytesleft - n;
    }
    return n==-1?-1:0; // -1 on failure, 0 on success
}

int sendall_file(int sock, int fd, off_t *offset, size_t count ){

    size_t total = 0;
    ssize_t sent;

    while(total < count) {
        sent = sendfile(sock, fd, offset, count-total);
        if (sent == -1) {
            break;
        }
        total = total + sent;
    }
    return sent==-1?-1:0;

}

int send_header(Request *request, int status_code, char *status_phrase) {

    char header[MAXHEADERSIZE];
    char server_time[1000];
    time_t tmp_t;
    struct tm *tmp_st;
    int tmp_i;

    tmp_t = time(NULL);
    tmp_st = localtime(&tmp_t);
    strftime(server_time, sizeof(server_time), DATETIMEFORMAT, tmp_st);

    tmp_i = sprintf(header,"%s %d %s\r\nServer: %s\r\nDate: %s\r\nConnection: close",
                    QKSRV_HTTPVERSION, status_code, status_phrase,
                    QKSRV_NAME,
                    server_time);

    return sendall_buffer(request->sockfd, header, strlen(header));
}

void request_destroy(Request *request) {

    free(request->resource);
    free(request);
}


void request_init(Request *request, int sockfd) {

    char buf[MAXHEADERSIZE];
    char *pos, *cur;
    int count, i=0, k=0;
    char *method;
    char *http_version;
    char *resource;
    pos = buf;
    cur = buf;

    count = recv(sockfd, buf, MAXHEADERSIZE-1, 0);
    if (count == -1) {
        perror("recv");
        exit(1);
    }

    while(i < 3) {
        while(!isspace(*pos)) {
            k++;
            pos++;
        }

        switch(i) {
            case 0:
                 method = (char *)malloc(sizeof(char) * (k + 1));
                 // TODO: Check for successful allocation
                 strncpy(method, cur, k);
                 method[k] = '\0';
                 cur = ++pos;
                 printf("%s\n", method);
                 k = 0;
                 i++;
                 break;

            case 1:
                 // Resource
                 resource = (char *)malloc(sizeof(char) * (k + 1));
                 // TODO: Check for successful allocation
                 strncpy(resource, cur, k);
                 resource[k] = '\0';
                 cur = ++pos;
                 printf("%s\n", resource);
                 k = 0;
                 i++;
                 break;

            case 2:
                 // HTTP Version
                 http_version = (char *)malloc(sizeof(char) * (k + 1));
                 // TODO: Check for successful allocation
                 strncpy(http_version, cur, k);
                 http_version[k] = '\0';
                 cur = ++pos;
                 printf("%s\n", http_version);
                 k = 0;
                 i++;
                 break;

        }
    }

    switch(*method) {
        case 'G':
        case 'g':
            request->method = GET;
            break;

        default:
            // TODO: Unsupported Requests
            request->method = UNSUPPORTED;
    }

    request->resource = resource;

    if (!strncmp(http_version, "HTTP/1.1", 9)) {
        request->http_version = HTTP11;
    }
    else if (!strncmp(http_version, "HTTP/1.0", 9)) {
        request->http_version = HTTP10;
    }
    else {
        request->http_version = HTTP10;
    }

    request->sockfd = sockfd;

    free(http_version);
    free(method);
}


void request_process(Request *request) {
    // Check if method is OK
    // Check if HTTPVersion is OK? Or just accept as HTTP10 if broken
    // if resource is OK
    char buf[MAXHEADERSIZE];
    char tmp[1000];
    char *cur, *tmp_c;
    char *root_path;
    char *resource_path;
    char *real_resource_path;
    off_t offset = 0;
    int tmp_i;
    int len;
    int fd;
    struct stat sb;
    cur = buf;

    tmp_i = sprintf(cur, "%s ", QKSRV_HTTPVERSION);
    cur = buf + tmp_i;
    // Unsupported Method
    if (request->method == UNSUPPORTED) {
        tmp_i = sprintf(cur, "501 Not Implemented\r\n");
        len = strlen(buf);
        sendall_buffer(request->sockfd, buf, len);
        return;
    }

    // Check resource
    // Is this safe?
    resource_path = malloc(sizeof(char)*PATH_MAX);
    root_path = malloc(sizeof(char)*PATH_MAX);
    real_resource_path = malloc(sizeof(char)*PATH_MAX);
    root_path = getcwd(root_path, PATH_MAX);
    resource_path = strncpy(resource_path, root_path, PATH_MAX);
    resource_path[PATH_MAX-1] = '\0';
    len = strlen(root_path);
    strncat(resource_path, request->resource, PATH_MAX-len-1);
    tmp_c = realpath(resource_path, real_resource_path);

    // If realpath returns NULL we know it the resource doesn't exist
    if (tmp_c == NULL) {
        tmp_i = sprintf(cur, "404 File Not Found\r\nServer: Asd\r\nDate: Sat, 01 Feb 2014 21:01:57 GMT\r\nConnection: close\r\n\r\n404 Ra!");
        /*tmp_i = sprintf(cur, "404 File Not Found\r\n");*/
        len = strlen(buf);
        printf("\n%s\n", buf);
        sendall_buffer(request->sockfd, buf, len);
        close(request->sockfd);
        return;
    }

    if (!strncmp(real_resource_path, root_path, strlen(root_path))) {
        send_header(request, 200, "OK");
        // check if directory
        // send list
        // else send file
        stat(real_resource_path, &sb);
        if (S_ISREG(sb.st_mode)) {
            tmp_i = sprintf(tmp, "\r\nContent-Length: %ld\r\n\r\n", sb.st_size);
            sendall_buffer(request->sockfd, tmp, strlen(tmp));
            fd = open(real_resource_path, O_RDONLY);
            sendall_file(request->sockfd, fd, &offset, sb.st_size);
        }
        close(request->sockfd);
        /*return;*/
    }
    else {
        /* bad request path */
    }

    //
    // Check if folder
    //  check if index.html exists
    //   else directory listing
    //   send directory listing as index.html
    // if file
    //  send file
    //
    printf("Real Resource Path is\n%s\n", real_resource_path);
    free(root_path);
    free(real_resource_path);
    free(resource_path);
    return;
}


void handle_connection(int sockfd) {
    Request *request;
    request = malloc(sizeof(Request));
    request_init(request, sockfd);
    request_process(request);
    request_destroy(request);
    exit(0);
}


int main(void) {
    int sockfd, new_sockfd, numbytes;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size;
    int status, yes=1, i=0;

    setbuf(stdout, NULL);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // For use by bind
    if ((status = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "Error: getaddrinfo: %s\n", gai_strerror(status));
        return 1;
    }

    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("Error: Getting Socket descriptor: sockfd func");
            continue;
        }

        // For bind() Address already in use
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("Error: setsockopt func");
            continue;
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("Error: bind");
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "Error: Unable to bind to anything\n");
        return 2;
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, BACKLOG) == -1) {
        perror("Error: listen");
        exit(1);
    }

    /* Setting SIGCHLD disposition to SIG_IGN so I dont have to wait on them
     * might not work with other unices
     */
    if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
        perror("SIGCHLD Handler");
        exit(1);
    }

    while(1) {
        client_addr_size = sizeof(client_addr);
        printf("Waiting to accept\n");
        new_sockfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_size);
        if (new_sockfd == -1) {
            perror("Accept");
            continue;
        }

        printf("\nRequest Number: %d\n", ++i);
        /* httplib starts at new_sockfd
         * and closes it Or keeps it alive
         */
        if (!fork()) { // In Child
            close(sockfd);
            printf("Waiting to recv\n");
            handle_connection(new_sockfd);
        }
            /*exit(0);*/
        close(new_sockfd);
    }
}
