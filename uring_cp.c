#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <getopt.h>
#include "banner.h"
#define QD 32

struct slot {
    char  *buf;
    off_t  offset;
    size_t len;
    int    is_write;
};

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --src <file> --dst <file>\n", prog);
}

int main(int argc, char *argv[]) {
    print_banner();
    const char *src_path = NULL;
    const char *dst_path = NULL;

    static const struct option long_opts[] = {
        {"src", required_argument, NULL, 's'},
        {"dst", required_argument, NULL, 'd'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': src_path = optarg; break;
        case 'd': dst_path = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!src_path || !dst_path) {
        usage(argv[0]);
        return 1;
    }

    struct io_uring ring;
    struct slot slots[QD] = {0};
    
    int ret = io_uring_queue_init(QD, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-ret));
        return 1;
    }
    
    int src_fd = open(src_path, O_RDONLY);
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (src_fd < 0 || dst_fd < 0) { perror("open"); return 1; }
    
    struct stat st;
    if (fstat(src_fd, &st) < 0) { perror("fstat"); return 1; }
    off_t file_size = st.st_size;

    int count = 0;
    off_t chunk = (file_size + QD - 1) / QD;

    for (int i = 0; i < QD; i++) {
        off_t off = (off_t)i * chunk;
        if (off >= file_size) break; 

        size_t len = chunk;
        if (off + (off_t)len > file_size) len = file_size - off;   /* last chunk is shorter */

        slots[i].buf = malloc(len);
        slots[i].offset = off;
        slots[i].len = len;
        slots[i].is_write = 0;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_read(sqe, src_fd, slots[i].buf, len, slots[i].offset);
        io_uring_sqe_set_data(sqe, &slots[i]);

        count++;
    }

    io_uring_submit(&ring);
    while (count > 0) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);
        struct slot *s = io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        count--;

        if (res < 0) { fprintf(stderr, "I/O error: %s\n", strerror(-res)); break; }

        if (!s->is_write) {
            s->len = res;
            s->is_write = 1;
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_write(sqe, dst_fd, s->buf, s->len, s->offset);
            io_uring_sqe_set_data(sqe, s);
            count++;
            io_uring_submit(&ring);
        }
    }

    for (int i = 0; i < QD; i++) free(slots[i].buf);
    close(src_fd);
    close(dst_fd);
    io_uring_queue_exit(&ring);
    return 0;
}