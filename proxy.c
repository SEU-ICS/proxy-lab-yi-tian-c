#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CAHCE_NUM 10
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
typedef char string[MAXLINE];
typedef struct {
    string host;
    string port;
    string path;
}url_t;
typedef struct {
    string url;
    char content[MAX_OBJECT_SIZE];
    int content_size;
    int timestamp;
} cache_file_t;

typedef struct {
    int using_cache_num;
    cache_file_t cache_files[MAX_CAHCE_NUM];
} cache_t;

void init_cache();
int query_cache(rio_t* rio_p, string url);
int add_cache(string url, char* content, int content_size);
static cache_t cache;
static sem_t mutex, w;
static int readcnt, timestamp;


void* m_thread(void* vargp);
void do_and_get(rio_t* client_rio_p, string url);
int yi_parse_url(string url, url_t* url_info);
int parse_header(rio_t* client_rio_p, string header_info, string host);
int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);

    int listenfd, * connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);

    init_cache();
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = (int*)malloc(sizeof(int));
        *connfd = accept(listenfd, (SA*)&clientaddr, &clientlen);
        if (*connfd < 0) {
            fprintf(stderr, "Accept Error: %s\n", strerror(errno));
            continue;
        }
        pthread_create(&tid, NULL, m_thread, connfd);
    }
    close(listenfd);
    // printf("%s", user_agent_hdr);
    return 0;
}
void* m_thread(void* vargp) {
    pthread_detach(pthread_self());
    int client_fd = *((int*)vargp);
    free(vargp);
    rio_t client_rio;
    string buf;
    rio_readinitb(&client_rio, client_fd);
    if (rio_readlineb(&client_rio, buf, MAXLINE) <= 0) {
        fprintf(stderr, "Read request line error: %s\n", strerror(errno));
        close(client_fd);
        return NULL;
    }
    string method, url, http_version;
    if (sscanf(buf, "%s %s %s", method, url, http_version) != 3) {
        fprintf(stderr, "Parse request line error: %s\n", strerror(errno));
        close(client_fd);
        return NULL;
    }
    if (!strcasecmp(method, "GET")) {
        do_and_get(&client_rio, url);
    }
    close(client_fd);
    return NULL;
}
int yi_parse_url(string url, url_t* url_info) {
    const int http_prefix_len = strlen("http://");
    if (strncasecmp(url, "http://", http_prefix_len)) {
        fprintf(stderr, "Not http protocol: %s\n", url);
        return -1;
    }
    char* host_start = url + http_prefix_len;
    char* port_start = strchr(host_start, ':');
    char* path_start = strchr(host_start, '/');
    if (path_start == NULL) {
        return -1;
    }
    if (port_start == NULL) {
        *path_start = '\0';
        strcpy(url_info->host, host_start);
        strcpy(url_info->port, "80");
        *path_start = '/';
        strcpy(url_info->path, path_start);
    }
    else {
        *port_start = '\0';
        strcpy(url_info->host, host_start);
        *port_start = ':';
        *path_start = '\0';
        strcpy(url_info->port, port_start + 1);
        *path_start = '/';
        strcpy(url_info->path, path_start);
    }

    return 0;
}
int parse_header(rio_t* client_rio_p, string header_info, string host) {
    string buf;
    int has_host_flag = 0;
    while (1) {
        rio_readlineb(client_rio_p, buf, MAXLINE);
        if (strcmp(buf, "\r\n") == 0) {
            break;
        }
        if (!strncasecmp(buf, "Host:", strlen("Host:"))) {
            has_host_flag = 1;
        }
        if (!strncasecmp(buf, "Connection:", strlen("Connection:"))) {
            continue;
        }
        if (!strncasecmp(buf, "Proxy-Connection:", strlen("Proxy-Connection:"))) {
            continue;
        }
        if (!strncasecmp(buf, "User-Agent:", strlen("User-Agent:"))) {
            continue;
        }
        strcat(header_info, buf);
    }
    if (!has_host_flag) {
        sprintf(buf, "Host: %s\r\n", host);
        strcpy(header_info, buf);
    }
    strcat(header_info, "Connection: close\r\n");
    strcat(header_info, "Proxy-Connection: close\r\n");
    strcat(header_info, user_agent_hdr);
    // 添加结束行
    strcat(header_info, "\r\n");
    return 0;
}
void do_and_get(rio_t* client_rio_p, string url) {
    if (query_cache(client_rio_p, url)) {
        return;
    }
    url_t url_info;
    if (yi_parse_url(url, &url_info) < 0) {
        fprintf(stderr, "Parse url error\n");
        return;
    }
    string header_info;
    parse_header(client_rio_p, header_info, url_info.host);
    int server_fd = open_clientfd(url_info.host, url_info.port);
    if (server_fd < 0) {
        fprintf(stderr, "Open connect to %s:%s error\n", url_info.host, url_info.port);
        return;
    }
    rio_t server_rio;
    rio_readinitb(&server_rio, server_fd);
    string buf;
    sprintf(buf, "GET %s HTTP/1.0\r\n%s", url_info.path, header_info);
    if (rio_writen(server_fd, buf, strlen(buf)) != strlen(buf)) {
        fprintf(stderr, "Send request line and header error\n");
        close(server_fd);
        return;
    }
    int resp_total = 0, resp_current = 0;
    char file_cache[MAX_OBJECT_SIZE];
    int client_fd = client_rio_p->rio_fd;
    while ((resp_current = rio_readnb(&server_rio, buf, MAXLINE))) {
        if (resp_current < 0) {
            fprintf(stderr, "Read server response error\n");
            close(server_fd);
            return;
        }
        if (resp_total + resp_current < MAX_OBJECT_SIZE) {
            memcpy(file_cache + resp_total, buf, resp_current);
        }
        resp_total += resp_current;
        if (rio_writen(client_fd, buf, resp_current) != resp_current) {
            fprintf(stderr, "Send response to client error\n");
            close(server_fd);
            return;
        }
    }
    if (resp_total < MAX_OBJECT_SIZE) {
        add_cache(url, file_cache, resp_total);
    }
    close(server_fd);
    return;
}
void init_cache() {
    timestamp = 0;
    readcnt = 0;
    cache.using_cache_num = 0;
    sem_init(&mutex, 0, 1);
    sem_init(&w, 0, 1);
}

int query_cache(rio_t* rio_p, string url) {
    P(&mutex);
    readcnt++;
    if (readcnt == 1) {
        P(&w);
    }
    V(&mutex);
    int hit_flag = 0;
    for (int i = 0; i < MAX_CAHCE_NUM;i++) {
        if (!strcmp(cache.cache_files[i].url, url)) {
            P(&mutex);
            cache.cache_files[i].timestamp = timestamp++;
            V(&mutex);
            rio_writen(rio_p->rio_fd, cache.cache_files[i].content, cache.cache_files[i].content_size);
            hit_flag = 1;
            break;
        }
    }
    P(&mutex);
    readcnt--;
    if (readcnt == 0) {
        V(&w);
    }
    V(&mutex);
    if (hit_flag) {
        return 1;
    }
    return 0;
}
int add_cache(string url, char* content, int content_size) {
    P(&w);
    if (cache.using_cache_num == (MAX_CAHCE_NUM - 1)) {
        int oldest_index;
        int oldest_timestamp = timestamp;
        for (int i = 0;i < MAX_CAHCE_NUM;i++) {
            if (cache.cache_files[i].timestamp < oldest_timestamp) {
                oldest_timestamp = cache.cache_files[i].timestamp;
                oldest_index = i;
            }
        }
        strcpy(cache.cache_files[oldest_index].url, url);
        memcpy(cache.cache_files[oldest_index].content, content, content_size);
        cache.cache_files[oldest_index].content_size = content_size;
        P(&mutex);
        cache.cache_files[oldest_index].timestamp = timestamp++;
        V(&mutex);
    }
    else {
        strcpy(cache.cache_files[cache.using_cache_num].url, url);
        memcpy(cache.cache_files[cache.using_cache_num].content, content, content_size);
        cache.cache_files[cache.using_cache_num].content_size = content_size;
        P(&mutex);
        cache.cache_files[cache.using_cache_num].timestamp = timestamp++;
        V(&mutex);
        cache.using_cache_num++;
    }
    V(&w);
    return 0;
}
