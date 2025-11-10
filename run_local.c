/*
run_local.c

Small helper to spawn N local processes (./process) for a given input file.
Each process is started with arguments: ./process <id> <input_file>

Usage:
  ./run_local <input_file>

The program forks N children and waits for them to finish. Outputs from each
child are redirected to proc_<id>.out in the current directory.

Compile:
  gcc Lab01/run_local.c -o Lab01/run_local

*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }
    const char *input = argv[1];
    FILE *f = fopen(input, "r");
    if (!f) { perror("open input"); return 1; }
    int N;
    if (fscanf(f, "%d", &N) != 1) { fprintf(stderr, "bad input\n"); return 1; }
    fclose(f);
    if (N <= 0) { fprintf(stderr, "bad N\n"); return 1; }

    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            /* child */
            char outname[128];
            snprintf(outname, sizeof(outname), "proc_%d.out", i);
            int fd = open(outname, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            execl("./process", "./process", idstr, input, (char*)NULL);
            perror("execl");
            _exit(1);
        } else {
            /* parent: small sleep to reduce races in port binding */
            usleep(50000);
        }
    }

    /* wait for all children */
    int status;
    while (wait(&status) > 0) {
        /* loop until all children exit */
    }
    printf("All processes finished. See proc_*.out and log.txt\n");
    return 0;
}