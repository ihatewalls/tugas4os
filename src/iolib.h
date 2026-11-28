#ifndef IOLIB_H
#define IOLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define IOLIB_DEFAULT_GROUP   "B07"
#define IOLIB_DEFAULT_LOG_DIR "logs"
#define IOLIB_PATH_MAX        512
#define IOLIB_TS_LEN          32

// Menyimpan data memori dari /proc/meminfo (dalam kB)
typedef struct {
    unsigned long dirty_kb;
    unsigned long writeback_kb;
    unsigned long mem_total_kb;
} io_meminfo_t;

// Menyimpan statistik tulis dari /sys/block/<dev>/stat
typedef struct {
    unsigned long long write_ios;
    unsigned long long write_merges;
    unsigned long long write_sectors;
} io_blkstat_t;

// Menyimpan hasil perhitungan statistik latensi (dalam nanodetik)
typedef struct {
    size_t count;
    double avg_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
} io_stats_t;

// Menyimpan file pointer dan path untuk file log CSV
typedef struct {
    FILE *fp;
    char path[IOLIB_PATH_MAX];
} io_csv_t;

// --- Waktu ---
uint64_t io_now_ns(void);
uint64_t io_elapsed_ns(uint64_t start_ns, uint64_t end_ns);
int io_format_timestamp(char *buf, size_t buf_size);

// --- Metrik Kernel ---
int io_read_meminfo(io_meminfo_t *info);
int io_read_blkstat(const char *dev, io_blkstat_t *stat);
io_blkstat_t io_diff_blkstat(const io_blkstat_t *before, const io_blkstat_t *after);

// --- Statistik ---
int io_compute_stats(const uint64_t *samples, size_t count, io_stats_t *out);

// --- Log CSV & Environment ---
int io_ensure_log_dir(const char *log_dir);
int io_csv_open(io_csv_t *csv,
                const char *group_id,
                const char *program,
                const char *scenario,
                const char *timestamp);
int io_csv_write_row(io_csv_t *csv,
                     int run_id,
                     const char *scenario,
                     size_t op_index,
                     uint64_t latency_ns,
                     size_t bytes);
int io_csv_close(io_csv_t *csv);

int io_save_env_info(const char *group_id,
                     const char *media_dir,
                     const char *dev,
                     const char *timestamp,
                     char *out_path,
                     size_t out_path_size);

// --- Helper Cetak ke Terminal ---
void io_print_metrics_snapshot(const char *label,
                               const io_meminfo_t *mem,
                               const char *dev,
                               const io_blkstat_t *blk);
void io_print_blkstat_diff(const char *dev, const io_blkstat_t *diff);
void io_print_stats(const io_stats_t *stats);

#endif
