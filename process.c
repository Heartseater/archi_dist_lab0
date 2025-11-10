/*
process.c

C implementation of the Lamport-based distributed lock (minimal, for local testing).
Implements the pseudocode in the README:
 - Request: broadcast REQ and add own request to local queue
 - Receive Request: add to queue and reply ACK
 - Release: remove from queue and broadcast REL
 - Receive Release: remove matching request from queue
 - Grant condition: own request is at head of queue AND ACKs from all processes with lc >= request lc

Important:
 - This program CALLS the existing ./critical binary exactly as provided in the repo:
     ./critical <process id> <sleep duration>
   Do NOT modify critical.c.
 - Usage:
     ./process <id> <input_file>
   The same input_file is given to all processes. The first line of input_file is N.
 - Networking:
   Uses localhost TCP. For simplicity each outgoing message opens a short connection to the target
   (REQ/ACK/REL). Each process listens on BASE_PORT + pid.

Compile:
  gcc -pthread Lab01/process.c -o Lab01/process

*/
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#define BASE_PORT 50000
#define MAXLINE 4096
#define RETRY_USEC 100000
#define MAX_PEERS 128

int N = 0;
int my_pid = -1;

/* Lamport clock */
int lc = 0;
pthread_mutex_t lc_m = PTHREAD_MUTEX_INITIALIZER;
int inc_lc(void) {
    pthread_mutex_lock(&lc_m);
    lc++;
    int tmp = lc;
    pthread_mutex_unlock(&lc_m);
    return tmp;
}
int update_lc_on_receive(int remote_lc) {
    pthread_mutex_lock(&lc_m);
    if (remote_lc >= lc) lc = remote_lc + 1;
    int tmp = lc;
    pthread_mutex_unlock(&lc_m);
    return tmp;
}

/* Request queue ordered by (req_lc, req_pid) */
typedef struct ReqEntry {
    int req_lc;
    int req_pid;
    struct ReqEntry *next;
} ReqEntry;
ReqEntry *queue_head = NULL;
pthread_mutex_t queue_m = PTHREAD_MUTEX_INITIALIZER;

void queue_insert(int req_lc, int req_pid) {
    pthread_mutex_lock(&queue_m);
    ReqEntry **pp = &queue_head;
    while (*pp) {
        if ((*pp)->req_lc < req_lc) { pp = &(*pp)->next; continue; }
        if ((*pp)->req_lc == req_lc && (*pp)->req_pid < req_pid) { pp = &(*pp)->next; continue; }
        break;
    }
    ReqEntry *e = malloc(sizeof(ReqEntry));
    e->req_lc = req_lc; e->req_pid = req_pid; e->next = *pp;
    *pp = e;
    pthread_mutex_unlock(&queue_m);
}

void queue_remove(int req_lc, int req_pid) {
    pthread_mutex_lock(&queue_m);
    ReqEntry **pp = &queue_head;
    while (*pp) {
        if ((*pp)->req_lc == req_lc && (*pp)->req_pid == req_pid) {
            ReqEntry *to = *pp;
            *pp = to->next;
            free(to);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&queue_m);
}

int queue_head_is(int req_lc, int req_pid) {
    pthread_mutex_lock(&queue_m);
    int ok = 0;
    if (queue_head && queue_head->req_lc == req_lc && queue_head->req_pid == req_pid) ok = 1;
    pthread_mutex_unlock(&queue_m);
    return ok;
}

/* Track ACKs for current request: store last ack lc per peer */
int ack_lc[MAX_PEERS];
pthread_mutex_t ack_m = PTHREAD_MUTEX_INITIALIZER;
void set_ack(int from, int value) {
    if (from < 0 || from >= MAX_PEERS) return;
    pthread_mutex_lock(&ack_m);
    ack_lc[from] = value;
    pthread_mutex_unlock(&ack_m);
}
int all_acks_ge(int target_lc) {
    pthread_mutex_lock(&ack_m);
    for (int i = 0; i < N; ++i) {
        if (ack_lc[i] < target_lc) { pthread_mutex_unlock(&ack_m); return 0; }
    }
    pthread_mutex_unlock(&ack_m);
    return 1;
}

/* Track releases seen per process for Wait semantics */
int releases_seen[MAX_PEERS];
pthread_mutex_t rel_m = PTHREAD_MUTEX_INITIALIZER;
void inc_release_seen(int pid) {
    if (pid < 0 || pid >= MAX_PEERS) return;
    pthread_mutex_lock(&rel_m);
    releases_seen[pid] += 1;
    pthread_mutex_unlock(&rel_m);
}
int get_release_seen(int pid) {
    if (pid < 0 || pid >= MAX_PEERS) return 0;
    pthread_mutex_lock(&rel_m);
    int v = releases_seen[pid];
    pthread_mutex_unlock(&rel_m);
    return v;
}

/* send a short-lived message to peer 'pid' */
void send_short(int pid, const char *msg) {
    if (pid < 0 || pid >= N) return;
    struct sockaddr_in peeraddr;
    peeraddr.sin_family = AF_INET;
    peeraddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    peeraddr.sin_port = htons(BASE_PORT + pid);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    if (connect(s, (struct sockaddr*)&peeraddr, sizeof(peeraddr)) == 0) {
        size_t len = strlen(msg);
        ssize_t written = 0;
        while ((size_t)written < len) {
            ssize_t w = write(s, msg + written, len - written);
            if (w <= 0) break;
            written += w;
        }
        /* close connection */
        close(s);
    } else {
        close(s);
    }
}

/* Broadcast to all other processes (short connections) */
void broadcast_short(const char *msg) {
    for (int i = 0; i < N; ++i) {
        if (i == my_pid) continue;
        send_short(i, msg);
    }
}

/* Incoming connection reader */
void process_line(const char *line);

void *conn_reader(void *arg) {
    int s = *(int*)arg;
    free(arg);
    FILE *f = fdopen(s, "r");
    if (!f) { close(s); return NULL; }
    char buf[MAXLINE];
    /* process every newline-terminated line */
    while (fgets(buf, sizeof(buf), f)) {
        process_line(buf);
    }
    fclose(f);
    return NULL;
}

/* Server thread: accept incoming connections */
void *server_thread(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(BASE_PORT + my_pid);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(srv, 20) < 0) {
        perror("listen");
        exit(1);
    }
    while (1) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        int *p = malloc(sizeof(int));
        *p = c;
        pthread_t t;
        pthread_create(&t, NULL, conn_reader, p);
        pthread_detach(t);
    }
    return NULL;
}

/* Connector thread: open outgoing short-lived connection to notify peers with HELLO
   then spawn a reader for the outgoing socket so we also accept messages that peers
   send back on that connection (not strictly necessary for short-messaging design,
   but we do HELLO then close; readers already exist from accept path). */
void *connector_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < N; ++i) {
        if (i == my_pid) continue;
        struct sockaddr_in peeraddr;
        peeraddr.sin_family = AF_INET;
        peeraddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        peeraddr.sin_port = htons(BASE_PORT + i);
        while (1) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(s, (struct sockaddr*)&peeraddr, sizeof(peeraddr)) == 0) {
                char hello[64];
                snprintf(hello, sizeof(hello), "HELLO %d\n", my_pid);
                write(s, hello, strlen(hello));
                close(s);
                break;
            } else {
                close(s);
                usleep(RETRY_USEC);
            }
        }
    }
    return NULL;
}

/* parse and handle messages (HELLO / REQ / ACK / REL) */
void process_line(const char *line) {
    char type[16];
    int a,b,c,d;
    if (sscanf(line, "%15s %d %d %d %d", type, &a, &b, &c, &d) < 1) return;
    if (strcmp(type, "HELLO") == 0) {
        /* nothing to do for HELLO */
        (void)a;
    } else if (strcmp(type, "REQ") == 0) {
        int req_lc = a;
        int req_pid = b;
        update_lc_on_receive(req_lc);
        queue_insert(req_lc, req_pid);
        /* send ACK: "ACK <ack_lc> <from_pid> <for_req_lc> <for_req_pid>\n" */
        int mylc = inc_lc();
        char buf[MAXLINE];
        snprintf(buf, sizeof(buf), "ACK %d %d %d %d\n", mylc, my_pid, req_lc, req_pid);
        send_short(req_pid, buf);
    } else if (strcmp(type, "ACK") == 0) {
        int ack_l = a, from = b, for_req_lc = c, for_req_pid = d;
        update_lc_on_receive(ack_l);
        /* If ACK is for our current request, record it */
        if (for_req_pid == my_pid) set_ack(from, ack_l);
    } else if (strcmp(type, "REL") == 0) {
        int rel_lc = a, req_lc = b, req_pid = c;
        update_lc_on_receive(rel_lc);
        queue_remove(req_lc, req_pid);
        inc_release_seen(req_pid);
    }
}

/* Request -> wait -> execute critical -> release */
int do_request(int duration) {
    int my_req_lc = inc_lc();
    queue_insert(my_req_lc, my_pid);

    pthread_mutex_lock(&ack_m);
    for (int i = 0; i < N; ++i) ack_lc[i] = -1000000000;
    ack_lc[my_pid] = my_req_lc; /* self-ack */
    pthread_mutex_unlock(&ack_m);

    char msg[MAXLINE];
    snprintf(msg, sizeof(msg), "REQ %d %d\n", my_req_lc, my_pid);
    broadcast_short(msg);

    /* wait until head and all ACKs */
    while (1) {
        usleep(100000);
        if (!queue_head_is(my_req_lc, my_pid)) continue;
        if (all_acks_ge(my_req_lc)) break;
    }

    /* Granted: call critical (existing binary) exactly as required */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./critical %d %d", my_pid, duration);
    printf("[proc %d] entering critical (duration=%d)\n", my_pid, duration);
    fflush(stdout);
    int rc = system(cmd);
    (void)rc;

    /* Release */
    queue_remove(my_req_lc, my_pid);
    int rel_l = inc_lc();
    snprintf(msg, sizeof(msg), "REL %d %d %d\n", rel_l, my_req_lc, my_pid);
    broadcast_short(msg);
    inc_release_seen(my_pid);
    return 0;
}

/* Wait until other process has released at least once after we observed it (simple semantics) */
void do_wait(int other_pid) {
    int seen = get_release_seen(other_pid);
    while (1) {
        usleep(100000);
        int now = get_release_seen(other_pid);
        if (now > seen) break;
    }
}

/* Run instructions in input file for this process id */
void run_instructions(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("open input"); exit(1); }
    char *line = NULL;
    size_t len = 0;
    ssize_t r;
    /* skip first line */
    r = getline(&line, &len, f);
    (void)r;
    while ((r = getline(&line, &len, f)) != -1) {
        if (r <= 1) continue;
        int target; char cmd[64]; int arg;
        int parsed = sscanf(line, "%d %63s %d", &target, cmd, &arg);
        if (parsed < 2) continue;
        if (target != my_pid) continue;
        if (strcmp(cmd, "Lock") == 0) {
            int dur = (parsed >= 3) ? arg : 1;
            do_request(dur);
        } else if (strcmp(cmd, "Wait") == 0) {
            int other = (parsed >= 3) ? arg : 0;
            do_wait(other);
        }
    }
    free(line);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <id> <input_file>\n", argv[0]);
        return 1;
    }
    my_pid = atoi(argv[1]);
    const char *infile = argv[2];

    /* read N */
    FILE *f = fopen(infile, "r");
    if (!f) { perror("open"); return 1; }
    if (fscanf(f, "%d", &N) != 1) { fprintf(stderr, "bad input\n"); return 1; }
    fclose(f);
    if (N <= 0 || N > MAX_PEERS) { fprintf(stderr, "bad N\n"); return 1; }

    /* init release counters */
    for (int i = 0; i < MAX_PEERS; ++i) {
        releases_seen[i] = 0;
    }

    pthread_t srv;
    if (pthread_create(&srv, NULL, server_thread, NULL) != 0) {
        perror("pthread_create server");
        return 1;
    }

    usleep(200000); /* small delay to let servers bind */

    pthread_t con;
    if (pthread_create(&con, NULL, connector_thread, NULL) != 0) {
        perror("pthread_create connector");
        return 1;
    }

    /* Run instructions (blocks until finished) */
    run_instructions(infile);

    /* allow messages propagate, then exit */
    sleep(1);
    printf("[proc %d] finished, exiting\n", my_pid);
    fflush(stdout);
    return 0;
}