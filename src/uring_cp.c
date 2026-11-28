#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <getopt.h>
#include "banner.h"
#include "iolib.h"
#define QD 32

struct slot { // used for breaking down the request
    char *buf;
    off_t offset;
    size_t len;
    int is_write;
    uint64_t start_ns;
};

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --src <file> --dst <file> [--dev <blockdev>]\n", prog);
}
static int copy(int src_fd, int dst_fd, off_t file_size, uint64_t lat[QD], size_t bytes[QD], int *nops){
    struct io_uring ring; // open uring
    struct slot slots[QD]; // seperates the 32 requests
    memset(slots, 0, sizeof(slots));
    *nops = 0;
    
    int ret = io_uring_queue_init(QD, &ring, 0); // initializes the queue with 32 slots
    if (ret < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-ret));
        return 1;
    }

    int count = 0, status = 0; 
    off_t chunk = (file_size + QD - 1) / QD; // chunk size, file/32

    for (int i = 0; i < QD; i++) {
        off_t off = (off_t)i * chunk; 
        if (off >= file_size) break; 

        size_t len = chunk;
        if (off + (off_t)len > file_size) len = file_size - off;   // last chunk might be shorter

        slots[i].buf = malloc(len); // 
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring); // submission queue
        if (slots[i].buf == NULL || sqe == NULL) {
            fprintf(stderr, "failed to set up slot %d\n", i);
            status = -1;
            goto out;
        }
        slots[i].offset = off;
        slots[i].len = len;
        slots[i].start_ns = io_now_ns();

        io_uring_prep_read(sqe, src_fd, slots[i].buf, len, slots[i].offset); // making read request
        io_uring_sqe_set_data(sqe, &slots[i]); // saves the request in the array

        count++; // counts operations
    }

    *nops = count;
    io_uring_submit(&ring); // send all 32 requests at once

    while (count > 0) { // submissions = completions
        struct io_uring_cqe *cqe; // completion queue
        ret = io_uring_wait_cqe(&ring, &cqe); // wait for an entry
        if (ret < 0) { 
            fprintf(stderr, "wait_cqe: %s\n", strerror(-ret));
            status = -1;
            break;
        }
        struct slot *s = io_uring_cqe_get_data(cqe); // take data
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe); // marks seen
        count--; // decrement for the loop

        if (res < 0) { fprintf(stderr, "I/O error: %s\n", strerror(-res)); break; }

        if ((size_t)res != s->len) {
            fprintf(stderr, "short %s: %d of %zu bytes\n",
                    s->is_write ? "write" : "read", res, s->len);
            status = -1;
            break;
        }

        if (!s->is_write) { // filters previous read completions from recent write completions
            s->is_write = 1;
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_write(sqe, dst_fd, s->buf, s->len, s->offset); 
            io_uring_sqe_set_data(sqe, s);
            count++;
            io_uring_submit(&ring);
        } else {
            int idx = (int)(s - slots);
            lat[idx] = io_elapsed_ns(s->start_ns, io_now_ns()); // saves how long it took to write
            bytes[idx] = s->len; // saves how many bytes were written 
        }
    }
    out:
        for (int i = 0; i < QD; i++) free(slots[i].buf); // free memory
        io_uring_queue_exit(&ring); // close uring
        return status;
}
int main(int argc, char *argv[]) {
    print_banner();
    const char *src_path = NULL; // file to be copied
    const char *dst_path = NULL; // copy
    const char *dev = NULL; // block to read 
    const char *scenario = "copy"; // for log
    int run_id = 1; // for log

    static const struct option long_opts[] = {
        {"src", required_argument, NULL, 's'}, // labels src argument as s
        {"dst", required_argument, NULL, 'd'}, // labels dst argument as d
        {"dev", required_argument, NULL, 'v'}, // labels dev argument as v
        {0, 0, 0, 0}
    };

    int opt; 
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) { // label from before
        case 's': src_path = optarg; break; // take source from --src argument
        case 'd': dst_path = optarg; break; // take destination from --dst argument
        case 'v': dev = optarg; break; // take dev from --dev argument
        default:  usage(argv[0]); return 1; // improper usage
        }
    }

    if (!src_path || !dst_path) { // required arguments
        usage(argv[0]);
        return 1;
    }

    char ts[IOLIB_TS_LEN]; // gets current timestamp 
    if (io_format_timestamp(ts, sizeof(ts)) != 0) { 
        fprintf(stderr, "failed to format timestamp\n");
        return 1;
    }

    char dst_copy[IOLIB_PATH_MAX]; // gets current directory
    snprintf(dst_copy, sizeof(dst_copy), "%s", dst_path);
    const char *media_dir = dirname(dst_copy);

    char env_path[IOLIB_PATH_MAX];
    if (io_save_env_info(NULL, media_dir, dev, ts, env_path, sizeof(env_path)) != 0)
        fprintf(stderr, "warning: could not write env file\n");
    else
        printf("Env file    : %s\n", env_path);

    int src_fd = open(src_path, O_RDONLY); // opens the src file
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644); // opens the dst file
    if (src_fd < 0 || dst_fd < 0) { perror("open"); return 1; } 
    
    struct stat st;  // used for getting file size
    if (fstat(src_fd, &st) < 0) { perror("fstat"); return 1; }
    off_t file_size = st.st_size;
    
    // snapshot before
    io_meminfo_t mem_b, mem_a;
    io_blkstat_t blk_b, blk_a;
    int have_mem = (io_read_meminfo(&mem_b) == 0);
    int have_blk = (dev != NULL && io_read_blkstat(dev, &blk_b) == 0);
    io_print_metrics_snapshot("Sebelum", have_mem ? &mem_b : NULL,
                              dev, have_blk ? &blk_b : NULL);


    uint64_t lat[QD] = {0};
    size_t bytes[QD] = {0};
    int nops = 0;

    // checks time before and after to find elapsed time
    uint64_t t0 = io_now_ns();
    int status = copy(src_fd, dst_fd, file_size, lat, bytes, &nops);
    uint64_t t1 = io_now_ns();

    close(src_fd); // copy done, fds can be closed
    close(dst_fd);
    if (status != 0) {
        fprintf(stderr, "copy failed, no results written\n");
        return 1;
    }

    // snapshot after
    have_mem = have_mem && (io_read_meminfo(&mem_a) == 0);
    int have_blk_a = have_blk && (io_read_blkstat(dev, &blk_a) == 0);
    io_print_metrics_snapshot("Sesudah", have_mem ? &mem_a : NULL, dev, have_blk_a ? &blk_a : NULL);
    if (have_blk_a) {
        io_blkstat_t diff = io_diff_blkstat(&blk_b, &blk_a);
        io_print_blkstat_diff(dev, &diff);
    }

    // results
    double secs = (double)io_elapsed_ns(t0, t1) / 1e9;
    double mb = (double)file_size / (1024.0 * 1024.0);
    printf("Waktu total : %.3f s\n", secs);
    printf("Throughput  : %.2f MB/s\n", secs > 0 ? mb / secs : 0.0);

    io_stats_t stats;
    if (nops > 0 && io_compute_stats(lat, (size_t)nops, &stats) == 0)
        io_print_stats(&stats);

    // csv
    io_csv_t csv;
    if (io_csv_open(&csv, NULL, "uring_cp", scenario, ts) != 0) {
        fprintf(stderr, "could not open CSV\n");
        return 1;
    }
    for (int i = 0; i < nops; i++)
        io_csv_write_row(&csv, run_id, scenario, (size_t)i, lat[i], bytes[i]);
    io_csv_close(&csv);
    printf("CSV         : %s\n", csv.path);

    return 0;
}
