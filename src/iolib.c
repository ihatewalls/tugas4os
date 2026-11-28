#define _POSIX_C_SOURCE 200809L

#include "iolib.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>

// Menghapus awalan "/dev/" kalau user mengoper "/dev/loop0"
static const char *clean_dev_name(const char *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    if (strncmp(dev, "/dev/", 5) == 0) {
        return dev + 5;
    }
    return dev;
}

// Membaca baris pertama dari sebuah file teks (misalnya file di /proc atau /sys)
static int read_first_line(const char *path, char *buf, size_t buf_size)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    if (fgets(buf, (int)buf_size, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // Hapus newline (\n) di akhir string
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[len - 1] = '\0';
        len--;
    }
    return 0;
}

// Fungsi pembanding untuk qsort() (mirip compareTo di Java)
static int compare_u64(const void *a, const void *b)
{
    uint64_t val_a = *(const uint64_t *)a;
    uint64_t val_b = *(const uint64_t *)b;

    if (val_a < val_b) {
        return -1;
    }
    if (val_a > val_b) {
        return 1;
    }
    return 0;
}

// Mengambil nilai persentil (p50, p95, p99) dari array yang sudah terurut
static uint64_t get_percentile(const uint64_t *sorted, size_t count, double percent)
{
    double pos = ((double)(count - 1) * percent) / 100.0;
    size_t index = (size_t)(pos + 0.5);
    if (index >= count) {
        index = count - 1;
    }
    return sorted[index];
}

// ============================================================
// 1. PENGUKURAN WAKTU
// ============================================================

uint64_t io_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    // Ubah detik ke nanodetik lalu tambahkan bagian nanodetiknya
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t io_elapsed_ns(uint64_t start_ns, uint64_t end_ns)
{
    if (end_ns < start_ns) {
        return 0;
    }
    return end_ns - start_ns;
}

int io_format_timestamp(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 16) {
        return -1;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) == NULL) {
        return -1;
    }

    if (strftime(buf, buf_size, "%Y%m%d-%H%M%S", &tm_now) == 0) {
        return -1;
    }
    return 0;
}

// ============================================================
// 2. PEMBACAAN METRIK KERNEL
// ============================================================

int io_read_meminfo(io_meminfo_t *info)
{
    if (info == NULL) {
        return -1;
    }

    info->dirty_kb = 0;
    info->writeback_kb = 0;
    info->mem_total_kb = 0;

    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) {
        return -1;
    }

    char line[256];
    int got_dirty = 0;
    int got_writeback = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long value = 0;
        if (sscanf(line, "MemTotal: %lu kB", &value) == 1) {
            info->mem_total_kb = value;
        } else if (sscanf(line, "Dirty: %lu kB", &value) == 1) {
            info->dirty_kb = value;
            got_dirty = 1;
        } else if (sscanf(line, "Writeback: %lu kB", &value) == 1) {
            info->writeback_kb = value;
            got_writeback = 1;
        }
    }

    fclose(fp);

    if (got_dirty == 0 || got_writeback == 0) {
        return -1;
    }
    return 0;
}

int io_read_blkstat(const char *dev, io_blkstat_t *stat)
{
    if (dev == NULL || dev[0] == '\0' || stat == NULL) {
        return -1;
    }

    stat->write_ios = 0;
    stat->write_merges = 0;
    stat->write_sectors = 0;

    const char *name = clean_dev_name(dev);
    char path[IOLIB_PATH_MAX];

    // Coba /sys/block/<dev>/stat dulu (untuk loop0, sda, dll)
    snprintf(path, sizeof(path), "/sys/block/%s/stat", name);
    FILE *fp = fopen(path, "r");

    // Kalau tidak ketemu (misalnya partisi seperti sda2), coba /sys/class/block/<dev>/stat
    if (fp == NULL) {
        snprintf(path, sizeof(path), "/sys/class/block/%s/stat", name);
        fp = fopen(path, "r");
        if (fp == NULL) {
            return -1;
        }
    }

    // Format kolom stat Linux:
    // 1:read_ios 2:read_merges 3:read_sectors 4:read_ticks
    // 5:write_ios 6:write_merges 7:write_sectors
    unsigned long long r_ios, r_merges, r_sectors, r_ticks;
    unsigned long long w_ios, w_merges, w_sectors;

    int count = fscanf(fp, "%llu %llu %llu %llu %llu %llu %llu",
                       &r_ios, &r_merges, &r_sectors, &r_ticks,
                       &w_ios, &w_merges, &w_sectors);
    fclose(fp);

    if (count < 7) {
        return -1;
    }

    stat->write_ios = w_ios;
    stat->write_merges = w_merges;
    stat->write_sectors = w_sectors;
    return 0;
}

io_blkstat_t io_diff_blkstat(const io_blkstat_t *before, const io_blkstat_t *after)
{
    io_blkstat_t diff = {0, 0, 0};

    if (before == NULL || after == NULL) {
        return diff;
    }

    if (after->write_ios >= before->write_ios) {
        diff.write_ios = after->write_ios - before->write_ios;
    }
    if (after->write_merges >= before->write_merges) {
        diff.write_merges = after->write_merges - before->write_merges;
    }
    if (after->write_sectors >= before->write_sectors) {
        diff.write_sectors = after->write_sectors - before->write_sectors;
    }
    return diff;
}

// ============================================================
// 3. PERHITUNGAN STATISTIK LATENSI
// ============================================================

int io_compute_stats(const uint64_t *samples, size_t count, io_stats_t *out)
{
    if (samples == NULL || count == 0 || out == NULL) {
        return -1;
    }

    // Salin array dulu supaya urutan array asli pemanggil tidak berubah
    uint64_t *sorted = malloc(count * sizeof(uint64_t));
    if (sorted == NULL) {
        return -1;
    }

    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sorted[i] = samples[i];
        sum += (double)samples[i];
    }

    // Urutkan dari kecil ke besar untuk mencari min, max, dan persentil
    qsort(sorted, count, sizeof(uint64_t), compare_u64);

    out->count = count;
    out->avg_ns = sum / (double)count;
    out->min_ns = sorted[0];
    out->max_ns = sorted[count - 1];
    out->p50_ns = get_percentile(sorted, count, 50.0);
    out->p95_ns = get_percentile(sorted, count, 95.0);
    out->p99_ns = get_percentile(sorted, count, 99.0);

    free(sorted);
    return 0;
}

// ============================================================
// 4. PENCATATAN LOG CSV & SPESIFIKASI LINGKUNGAN
// ============================================================

int io_ensure_log_dir(const char *log_dir)
{
    const char *dir = log_dir;
    if (dir == NULL || dir[0] == '\0') {
        dir = IOLIB_DEFAULT_LOG_DIR;
    }

    // mkdir mengembalikan 0 kalau berhasil, atau -1 dengan errno == EEXIST kalau sudah ada
    if (mkdir(dir, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    return -1;
}

int io_csv_open(io_csv_t *csv,
                const char *group_id,
                const char *program,
                const char *scenario,
                const char *timestamp)
{
    if (csv == NULL || program == NULL || scenario == NULL) {
        return -1;
    }

    csv->fp = NULL;
    csv->path[0] = '\0';

    if (io_ensure_log_dir(IOLIB_DEFAULT_LOG_DIR) != 0) {
        return -1;
    }

    const char *grp = group_id;
    if (grp == NULL || grp[0] == '\0') {
        grp = IOLIB_DEFAULT_GROUP;
    }

    char ts_buf[IOLIB_TS_LEN];
    const char *ts = timestamp;
    if (ts == NULL || ts[0] == '\0') {
        if (io_format_timestamp(ts_buf, sizeof(ts_buf)) != 0) {
            return -1;
        }
        ts = ts_buf;
    }

    snprintf(csv->path, sizeof(csv->path), "%s/%s-%s-%s-%s.csv",
             IOLIB_DEFAULT_LOG_DIR, grp, program, scenario, ts);

    csv->fp = fopen(csv->path, "w");
    if (csv->fp == NULL) {
        return -1;
    }

    // Tulis baris header CSV
    fprintf(csv->fp, "run_id,scenario,op_index,latency_ns,bytes\n");
    return 0;
}

int io_csv_write_row(io_csv_t *csv,
                     int run_id,
                     const char *scenario,
                     size_t op_index,
                     uint64_t latency_ns,
                     size_t bytes)
{
    if (csv == NULL || csv->fp == NULL || scenario == NULL) {
        return -1;
    }

    int written = fprintf(csv->fp, "%d,%s,%zu,%llu,%zu\n",
                          run_id, scenario, op_index,
                          (unsigned long long)latency_ns, bytes);
    if (written < 0) {
        return -1;
    }
    return 0;
}

int io_csv_close(io_csv_t *csv)
{
    if (csv == NULL || csv->fp == NULL) {
        return 0;
    }
    int status = fclose(csv->fp);
    csv->fp = NULL;
    return status;
}

int io_save_env_info(const char *group_id,
                     const char *media_dir,
                     const char *dev,
                     const char *timestamp,
                     char *out_path,
                     size_t out_path_size)
{
    if (io_ensure_log_dir(IOLIB_DEFAULT_LOG_DIR) != 0) {
        return -1;
    }

    const char *grp = group_id;
    if (grp == NULL || grp[0] == '\0') {
        grp = IOLIB_DEFAULT_GROUP;
    }

    char ts_buf[IOLIB_TS_LEN];
    const char *ts = timestamp;
    if (ts == NULL || ts[0] == '\0') {
        if (io_format_timestamp(ts_buf, sizeof(ts_buf)) != 0) {
            return -1;
        }
        ts = ts_buf;
    }

    char path[IOLIB_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s-env-%s.txt",
             IOLIB_DEFAULT_LOG_DIR, grp, ts);

    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return -1;
    }

    // 1. Versi Kernel (sama seperti uname -r)
    struct utsname uts;
    if (uname(&uts) == 0) {
        fprintf(fp, "Kernel : %s\n", uts.release);
    } else {
        fprintf(fp, "Kernel : unknown\n");
    }

    // 2. Total RAM (dari /proc/meminfo)
    io_meminfo_t mem;
    if (io_read_meminfo(&mem) == 0 && mem.mem_total_kb > 0) {
        double ram_gb = (double)mem.mem_total_kb / (1024.0 * 1024.0);
        fprintf(fp, "RAM : %.1f GB\n", ram_gb);
    } else {
        fprintf(fp, "RAM : unknown\n");
    }

    // 3. Informasi Media (memakai perintah df -hT)
    const char *dev_name = clean_dev_name(dev);
    if (media_dir != NULL && media_dir[0] != '\0') {
        char cmd[IOLIB_PATH_MAX + 64];
        snprintf(cmd, sizeof(cmd), "df -hT '%s' 2>/dev/null", media_dir);

        char fs_dev[128] = "unknown";
        char fs_type[64] = "unknown";
        char fs_size[64] = "unknown";

        FILE *df_pipe = popen(cmd, "r");
        if (df_pipe != NULL) {
            char line[512];
            // Baris 1 adalah header df, baris 2 adalah datanya
            if (fgets(line, sizeof(line), df_pipe) != NULL &&
                fgets(line, sizeof(line), df_pipe) != NULL) {
                sscanf(line, "%127s %63s %63s", fs_dev, fs_type, fs_size);
            }
            pclose(df_pipe);
        }

        const char *shown_dev = (dev_name != NULL && dev_name[0] != '\0') ? dev_name : "unknown";
        fprintf(fp, "Media : %s (%s %s %s) [dev=%s]\n",
                media_dir, fs_dev, fs_type, fs_size, shown_dev);
    } else {
        fprintf(fp, "Media : N/A\n");
    }

    // 4. Parameter VM dirty_ratio & dirty_background_ratio
    char val[64];
    if (read_first_line("/proc/sys/vm/dirty_ratio", val, sizeof(val)) == 0) {
        fprintf(fp, "vm.dirty_ratio : %s\n", val);
    } else {
        fprintf(fp, "vm.dirty_ratio : unknown\n");
    }

    if (read_first_line("/proc/sys/vm/dirty_background_ratio", val, sizeof(val)) == 0) {
        fprintf(fp, "vm.dirty_background_ratio : %s\n", val);
    } else {
        fprintf(fp, "vm.dirty_background_ratio : unknown\n");
    }

    // 5. Read-ahead (dalam kB)
    int got_ra = 0;
    if (dev_name != NULL && dev_name[0] != '\0') {
        char ra_path[IOLIB_PATH_MAX];
        snprintf(ra_path, sizeof(ra_path), "/sys/block/%s/queue/read_ahead_kb", dev_name);
        if (read_first_line(ra_path, val, sizeof(val)) == 0) {
            got_ra = 1;
        } else {
            // Untuk partisi seperti sda2, queue ada di direktori induknya (sda)
            snprintf(ra_path, sizeof(ra_path), "/sys/class/block/%s/../queue/read_ahead_kb", dev_name);
            if (read_first_line(ra_path, val, sizeof(val)) == 0) {
                got_ra = 1;
            }
        }
    }
    if (got_ra) {
        fprintf(fp, "Read-ahead : %s kB\n", val);
    } else {
        fprintf(fp, "Read-ahead : unknown\n");
    }

    // 6. Output lsblk
    fprintf(fp, "lsblk :\n");
    FILE *lsblk_pipe = popen("lsblk 2>&1", "r");
    if (lsblk_pipe != NULL) {
        char line[512];
        while (fgets(line, sizeof(line), lsblk_pipe) != NULL) {
            fprintf(fp, "  %s", line);
        }
        pclose(lsblk_pipe);
    }

    fclose(fp);

    if (out_path != NULL && out_path_size > 0) {
        snprintf(out_path, out_path_size, "%s", path);
    }
    return 0;
}

// ============================================================
// 5. HELPER CETAK KE TERMINAL
// ============================================================

void io_print_metrics_snapshot(const char *label,
                               const io_meminfo_t *mem,
                               const char *dev,
                               const io_blkstat_t *blk)
{
    if (label != NULL) {
        printf("[%s]\n", label);
    }
    if (mem != NULL) {
        printf("Dirty       : %lu kB\n", mem->dirty_kb);
        printf("Writeback   : %lu kB\n", mem->writeback_kb);
    }
    if (blk != NULL) {
        const char *name = (dev != NULL) ? clean_dev_name(dev) : "dev";
        printf("%-11s : write ios %llu | write merges %llu | write sectors %llu\n",
               name, blk->write_ios, blk->write_merges, blk->write_sectors);
    }
}

void io_print_blkstat_diff(const char *dev, const io_blkstat_t *diff)
{
    if (diff == NULL) {
        return;
    }
    const char *name = (dev != NULL) ? clean_dev_name(dev) : "dev";
    printf("Selisih %-4s: write ios +%llu | write merges +%llu | write sectors +%llu\n",
           name, diff->write_ios, diff->write_merges, diff->write_sectors);
}

void io_print_stats(const io_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    printf("Latensi (ns): avg %.0f | min %llu | max %llu | p50 %llu | p95 %llu | p99 %llu\n",
           stats->avg_ns,
           (unsigned long long)stats->min_ns,
           (unsigned long long)stats->max_ns,
           (unsigned long long)stats->p50_ns,
           (unsigned long long)stats->p95_ns,
           (unsigned long long)stats->p99_ns);
}
