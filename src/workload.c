#define _POSIX_C_SOURCE 200809L

#include "banner.h"
#include "iolib.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// I/O
typedef enum
{
    MODE_SEQWRITE = 0,
    MODE_RANDWRITE,
    MODE_SEQREAD,
    MODE_RANDREAD,
    MODE_INVALID
} WorkloadMode;

// Menyimpan seluruh input argumen
typedef struct
{
    const char *dir;
    size_t size_mb;
    size_t bs_kb;
    size_t ops;
    WorkloadMode mode;
    const char *mode_str;
    bool use_fsync;
    const char *dev;
    int run_id;
    const char *label;
    uint64_t seed;
} WorkloadConfig;

// Menampilkan menu
static void print_workload_usage(const char *prog_name)
{
    printf("Penggunaan: %s [OPSI]\n\n", prog_name);
    printf("Opsi:\n");
    printf("  --dir <path>      Direktori target (Media A atau Media B) [default: .]\n");
    printf("  --size <MB>       Ukuran berkas uji dalam MiB [default: 32]\n");
    printf("  --bs <KB>         Ukuran blok operasi dalam KiB [default: 4]\n");
    printf("  --mode <mode>     Mode I/O: seqwrite | randwrite | seqread | randread [default: seqwrite]\n");
    printf("  --ops <N>         Jumlah operasi tepat (alias: --count; 0 = otomatis) [default: 0]\n");
    printf("  --fsync           Panggil fsync() sekali di akhir penulisan\n");
    printf("  --dev <nama>      Nama perangkat blok di /sys/block/<dev>/stat (mis. loop0, vda)\n");
    printf("  --run-id <N>      ID pengulangan untuk kolom run_id di CSV [default: 1]\n");
    printf("  --label <teks>    Label skenario pada CSV & nama berkas log\n");
    printf("  --seed <N>        Seed PRNG untuk pola acak [default: 42]\n");
    printf("  -h, --help        Tampilkan pesan bantuan ini\n");
}

static WorkloadMode parse_mode(const char *s)
{
    if (!s)
        return MODE_INVALID;
    if (strcmp(s, "seqwrite") == 0)
        return MODE_SEQWRITE;
    if (strcmp(s, "randwrite") == 0)
        return MODE_RANDWRITE;
    if (strcmp(s, "seqread") == 0)
        return MODE_SEQREAD;
    if (strcmp(s, "randread") == 0)
        return MODE_RANDREAD;
    return MODE_INVALID;
}

static bool is_write_mode(WorkloadMode mode)
{
    return mode == MODE_SEQWRITE || mode == MODE_RANDWRITE;
}

static int parse_u64_arg(const char *str, uint64_t *out)
{
    if (!str || *str == '\0' || *str == '-')
    {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0')
    {
        return -1;
    }
    *out = (uint64_t)val;
    return 0;
}

static uint64_t xorshift64_star(uint64_t *state)
{
    uint64_t x = *state;
    if (x == 0)
    {
        x = 0x9E3779B97F4A7C15ULL;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void fill_pattern_buffer(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t s = seed ^ 0xA5A55A5A12345678ULL;
    size_t i = 0;
    while (i + sizeof(uint64_t) <= len)
    {
        uint64_t v = xorshift64_star(&s);
        memcpy(buf + i, &v, sizeof(uint64_t));
        i += sizeof(uint64_t);
    }
    if (i < len)
    {
        uint64_t v = xorshift64_star(&s);
        memcpy(buf + i, &v, len - i);
    }
    if (len > 0 && buf[0] == 0)
    {
        buf[0] = 0x5A;
    }
}

static int write_full(int fd, const uint8_t *buf, size_t count)
{
    size_t done = 0;
    while (done < count)
    {
        ssize_t n = write(fd, buf + done, count - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
        {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int pwrite_full(int fd, const uint8_t *buf, size_t count, off_t offset)
{
    size_t done = 0;
    while (done < count)
    {
        ssize_t n = pwrite(fd, buf + done, count - done, offset + (off_t)done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
        {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int read_full(int fd, uint8_t *buf, size_t count)
{
    size_t done = 0;
    while (done < count)
    {
        ssize_t n = read(fd, buf + done, count - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
        {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int pread_full(int fd, uint8_t *buf, size_t count, off_t offset)
{
    size_t done = 0;
    while (done < count)
    {
        ssize_t n = pread(fd, buf + done, count - done, offset + (off_t)done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
        {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int ensure_test_file_ready(const char *filepath, size_t min_bytes, size_t create_bytes, uint64_t seed)
{
    struct stat st;
    if (stat(filepath, &st) == 0 && st.st_size >= 0 && (size_t)st.st_size >= min_bytes)
    {
        return 0;
    }

    printf("[Info] Menyiapkan berkas uji %s (%zu MB) sebelum pengukuran...\n",
           filepath, create_bytes / ((size_t)1024 * (size_t)1024));

    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
    {
        fprintf(stderr, "Gagal membuat berkas uji %s: %s\n", filepath, strerror(errno));
        return -1;
    }

    const size_t chunk_size = 65536;
    uint8_t *chunk = malloc(chunk_size);
    if (!chunk)
    {
        close(fd);
        fprintf(stderr, "Gagal mengalokasikan buffer persiapan berkas.\n");
        return -1;
    }
    fill_pattern_buffer(chunk, chunk_size, seed);

    size_t remaining = create_bytes;
    int rc = 0;
    while (remaining > 0)
    {
        size_t to_write = remaining < chunk_size ? remaining : chunk_size;
        if (write_full(fd, chunk, to_write) != 0)
        {
            fprintf(stderr, "Gagal menulis berkas uji %s: %s\n", filepath, strerror(errno));
            rc = -1;
            break;
        }
        remaining -= to_write;
    }

    if (rc == 0 && fsync(fd) != 0)
    {
        fprintf(stderr, "Gagal fsync saat persiapan %s: %s\n", filepath, strerror(errno));
        rc = -1;
    }

    free(chunk);
    close(fd);
    return rc;
}

static int parse_args(int argc, char **argv, WorkloadConfig *cfg, bool *show_help)
{
    static const struct option long_opts[] = {
        {"dir", required_argument, NULL, 'd'},
        {"size", required_argument, NULL, 's'},
        {"bs", required_argument, NULL, 'b'},
        {"mode", required_argument, NULL, 'm'},
        {"ops", required_argument, NULL, 'o'},
        {"count", required_argument, NULL, 'o'},
        {"fsync", no_argument, NULL, 'f'},
        {"dev", required_argument, NULL, 'v'},
        {"run-id", required_argument, NULL, 'r'},
        {"label", required_argument, NULL, 'l'},
        {"seed", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    // Default value
    cfg->dir = ".";
    cfg->size_mb = 32;
    cfg->bs_kb = 4;
    cfg->ops = 0;
    cfg->mode_str = "seqwrite";
    cfg->mode = MODE_SEQWRITE;
    cfg->use_fsync = false;
    cfg->dev = NULL;
    cfg->run_id = 1;
    cfg->label = NULL;
    cfg->seed = 42ULL;
    *show_help = false;

    int opt;
    int opt_idx = 0;
    while ((opt = getopt_long(argc, argv, "d:s:b:m:o:fv:r:l:e:h", long_opts, &opt_idx)) != -1)
    {
        uint64_t val = 0;
        switch (opt)
        {
        case 'd':
            cfg->dir = optarg;
            break;
        case 's':
            if (parse_u64_arg(optarg, &val) != 0 || val == 0 || val > 65536ULL)
            {
                fprintf(stderr, "Error: --size harus bilangan bulat antara 1 dan 65536 MB.\n");
                return -1;
            }
            cfg->size_mb = (size_t)val;
            break;
        case 'b':
            if (parse_u64_arg(optarg, &val) != 0 || val == 0 || val > 65536ULL)
            {
                fprintf(stderr, "Error: --bs harus bilangan bulat antara 1 dan 65536 KB.\n");
                return -1;
            }
            cfg->bs_kb = (size_t)val;
            break;
        case 'm':
            cfg->mode_str = optarg;
            cfg->mode = parse_mode(optarg);
            if (cfg->mode == MODE_INVALID)
            {
                fprintf(stderr, "Error: --mode '%s' tidak valid (pilih seqwrite, randwrite, seqread, randread).\n",
                        optarg);
                return -1;
            }
            break;
        case 'o':
            if (parse_u64_arg(optarg, &val) != 0 || val > 10000000ULL)
            {
                fprintf(stderr, "Error: --ops harus bilangan bulat non-negatif (<= 10000000).\n");
                return -1;
            }
            cfg->ops = (size_t)val;
            break;
        case 'f':
            cfg->use_fsync = true;
            break;
        case 'v':
            cfg->dev = optarg;
            break;
        case 'r':
            if (parse_u64_arg(optarg, &val) != 0 || val == 0 || val > INT_MAX)
            {
                fprintf(stderr, "Error: --run-id harus bilangan bulat >= 1.\n");
                return -1;
            }
            cfg->run_id = (int)val;
            break;
        case 'l':
            if (!optarg || *optarg == '\0')
            {
                fprintf(stderr, "Error: --label tidak boleh kosong.\n");
                return -1;
            }
            cfg->label = optarg;
            break;
        case 'e':
            if (parse_u64_arg(optarg, &val) != 0)
            {
                fprintf(stderr, "Error: --seed harus bilangan bulat tak bertanda.\n");
                return -1;
            }
            cfg->seed = val;
            break;
        case 'h':
            *show_help = true;
            return 0;
        default:
            return -1;
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Error: argumen tidak dikenal '%s'. Gunakan --help untuk bantuan.\n",
                argv[optind]);
        return -1;
    }

    if (cfg->bs_kb > cfg->size_mb * 1024ULL)
    {
        fprintf(stderr, "Error: --bs (%zu KB) tidak boleh lebih besar dari --size (%zu MB).\n",
                cfg->bs_kb, cfg->size_mb);
        return -1;
    }

    struct stat dir_st;
    if (stat(cfg->dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode))
    {
        fprintf(stderr, "Error: direktori --dir '%s' tidak ditemukan atau bukan direktori.\n",
                cfg->dir);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    WorkloadConfig cfg;
    bool show_help = false;
    if (parse_args(argc, argv, &cfg, &show_help) != 0)
    {
        return 1;
    }
    if (show_help)
    {
        print_banner();
        print_workload_usage(argv[0]);
        return 0;
    }

    print_banner();

    char scenario[128];
    if (cfg.label)
    {
        snprintf(scenario, sizeof(scenario), "%s", cfg.label);
    }
    else if (cfg.use_fsync && is_write_mode(cfg.mode))
    {
        snprintf(scenario, sizeof(scenario), "%s_fsync", cfg.mode_str);
    }
    else
    {
        snprintf(scenario, sizeof(scenario), "%s", cfg.mode_str);
    }

    char timestamp[IOLIB_TS_LEN];
    if (io_format_timestamp(timestamp, sizeof(timestamp)) != 0)
    {
        snprintf(timestamp, sizeof(timestamp), "00000000-000000");
    }

    char env_path[IOLIB_PATH_MAX] = {0};
    if (io_save_env_info(IOLIB_DEFAULT_GROUP, cfg.dir, cfg.dev, timestamp,
                         env_path, sizeof(env_path)) != 0)
    {
        fprintf(stderr, "Peringatan: gagal menyimpan berkas environment ke %s.\n", IOLIB_DEFAULT_LOG_DIR);
    }

    // Hitung total blok dan alokasi buffer data + array latensi di RAM
    size_t file_bytes = cfg.size_mb * 1024ULL * 1024ULL;
    size_t bs_bytes = cfg.bs_kb * 1024ULL;
    size_t total_blocks = file_bytes / bs_bytes;
    size_t num_ops = (cfg.ops > 0) ? cfg.ops : total_blocks;

    char filepath[PATH_MAX];
    int path_len = snprintf(filepath, sizeof(filepath), "%s/workload_test.bin", cfg.dir);
    if (path_len < 0 || (size_t)path_len >= sizeof(filepath))
    {
        fprintf(stderr, "Error: path berkas uji terlalu panjang.\n");
        return 1;
    }

    uint8_t *buf = malloc(bs_bytes);
    uint64_t *latencies = calloc(num_ops, sizeof(uint64_t));
    if (!buf || !latencies)
    {
        fprintf(stderr, "Error: gagal mengalokasikan memori untuk buffer/latensi.\n");
        free(buf);
        free(latencies);
        return 1;
    }
    fill_pattern_buffer(buf, bs_bytes, cfg.seed);

    if (cfg.mode == MODE_SEQREAD || cfg.mode == MODE_RANDREAD || cfg.mode == MODE_RANDWRITE)
    {
        size_t min_required = (cfg.ops > 0 && (num_ops * bs_bytes) < file_bytes)
                                  ? (num_ops * bs_bytes)
                                  : file_bytes;
        if (ensure_test_file_ready(filepath, min_required, file_bytes, cfg.seed) != 0)
        {
            free(buf);
            free(latencies);
            return 1;
        }
        struct stat ready_st;
        if (stat(filepath, &ready_st) == 0 && ready_st.st_size > 0 && (size_t)ready_st.st_size < file_bytes)
        {
            total_blocks = (size_t)ready_st.st_size / bs_bytes;
            if (total_blocks == 0)
            {
                fprintf(stderr, "Error: ukuran berkas %s lebih kecil dari block size.\n", filepath);
                free(buf);
                free(latencies);
                return 1;
            }
        }
    }

    // Buka berkas sesuai mode
    int fd = -1;
    if (cfg.mode == MODE_SEQWRITE)
    {
        fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    }
    else if (cfg.mode == MODE_RANDWRITE)
    {
        fd = open(filepath, O_RDWR);
    }
    else
    {
        fd = open(filepath, O_RDONLY);
    }
    if (fd < 0)
    {
        fprintf(stderr, "Error: gagal membuka %s: %s\n", filepath, strerror(errno));
        free(buf);
        free(latencies);
        return 1;
    }

    io_meminfo_t mem_before = {0}, mem_after = {0};
    io_blkstat_t blk_before = {0}, blk_after = {0};
    bool has_mem_before = (io_read_meminfo(&mem_before) == 0);
    if (!has_mem_before)
    {
        fprintf(stderr, "Peringatan: gagal membaca /proc/meminfo sebelum beban.\n");
    }

    bool has_blk = false;
    if (cfg.dev && *cfg.dev)
    {
        if (io_read_blkstat(cfg.dev, &blk_before) == 0)
        {
            has_blk = true;
        }
        else
        {
            fprintf(stderr, "Peringatan: gagal membaca statistik /sys/block/%s/stat.\n", cfg.dev);
        }
    }

    // Jalankan I/O dan catat tiap operasi
    uint64_t rng_state = cfg.seed;
    uint64_t t_total_start = io_now_ns();
    uint64_t fsync_ns = 0;

    for (size_t i = 0; i < num_ops; i++)
    {
        int io_rc = 0;
        uint64_t t_op_start = io_now_ns();

        switch (cfg.mode)
        {
        case MODE_SEQWRITE:
            if (i > 0 && (i % total_blocks) == 0)
            {
                if (lseek(fd, 0, SEEK_SET) < 0)
                {
                    io_rc = -1;
                    break;
                }
            }
            io_rc = write_full(fd, buf, bs_bytes);
            break;

        case MODE_SEQREAD:
            if (i > 0 && (i % total_blocks) == 0)
            {
                if (lseek(fd, 0, SEEK_SET) < 0)
                {
                    io_rc = -1;
                    break;
                }
            }
            io_rc = read_full(fd, buf, bs_bytes);
            break;

        case MODE_RANDWRITE:
        {
            off_t off = (off_t)(xorshift64_star(&rng_state) % total_blocks) * (off_t)bs_bytes;
            io_rc = pwrite_full(fd, buf, bs_bytes, off);
            break;
        }

        case MODE_RANDREAD:
        {
            off_t off = (off_t)(xorshift64_star(&rng_state) % total_blocks) * (off_t)bs_bytes;
            io_rc = pread_full(fd, buf, bs_bytes, off);
            break;
        }

        default:
            io_rc = -1;
            errno = EINVAL;
            break;
        }

        uint64_t t_op_end = io_now_ns();
        if (io_rc != 0)
        {
            fprintf(stderr, "Error: operasi I/O ke-%zu gagal pada %s: %s\n",
                    i, filepath, strerror(errno));
            close(fd);
            free(buf);
            free(latencies);
            return 1;
        }
        latencies[i] = io_elapsed_ns(t_op_start, t_op_end);
    }

    if (cfg.use_fsync && is_write_mode(cfg.mode))
    {
        uint64_t t_fs_start = io_now_ns();
        if (fsync(fd) != 0)
        {
            fprintf(stderr, "Error: fsync() gagal pada %s: %s\n", filepath, strerror(errno));
            close(fd);
            free(buf);
            free(latencies);
            return 1;
        }
        fsync_ns = io_elapsed_ns(t_fs_start, io_now_ns());
    }

    uint64_t t_total_end = io_now_ns();
    close(fd);

    bool has_mem_after = (io_read_meminfo(&mem_after) == 0);
    if (!has_mem_after)
    {
        fprintf(stderr, "Peringatan: gagal membaca /proc/meminfo sesudah beban.\n");
    }
    if (has_blk && io_read_blkstat(cfg.dev, &blk_after) != 0)
    {
        fprintf(stderr, "Peringatan: gagal membaca statistik /sys/block/%s/stat sesudah beban.\n",
                cfg.dev);
        has_blk = false;
    }

    // Hitung statistik (avg, min, max, p50, p95, p99)
    io_stats_t stats = {0};
    if (io_compute_stats(latencies, num_ops, &stats) != 0)
    {
        fprintf(stderr, "Error: gagal menghitung statistik latensi.\n");
        free(buf);
        free(latencies);
        return 1;
    }

    // Write to CSV
    io_csv_t csv = {0};
    if (io_csv_open(&csv, IOLIB_DEFAULT_GROUP, "workload", scenario, timestamp) != 0)
    {
        fprintf(stderr, "Error: gagal membuka berkas CSV di %s.\n", IOLIB_DEFAULT_LOG_DIR);
        free(buf);
        free(latencies);
        return 1;
    }
    for (size_t i = 0; i < num_ops; i++)
    {
        if (io_csv_write_row(&csv, cfg.run_id, scenario, i, latencies[i], bs_bytes) != 0)
        {
            fprintf(stderr, "Error: gagal menulis baris CSV ke-%zu.\n", i);
            io_csv_close(&csv);
            free(buf);
            free(latencies);
            return 1;
        }
    }
    if (io_csv_close(&csv) != 0)
    {
        fprintf(stderr, "Peringatan: gagal menutup berkas CSV %s.\n", csv.path);
    }

    // Hitung throughput and Show Output
    uint64_t elapsed_ns = io_elapsed_ns(t_total_start, t_total_end);
    double elapsed_s = (double)elapsed_ns / 1e9;
    if (elapsed_s <= 0.0)
    {
        elapsed_s = 1e-9;
    }
    double total_mb = ((double)num_ops * (double)bs_bytes) / (1024.0 * 1024.0);
    double throughput_mbs = total_mb / elapsed_s;

    printf("Konfigurasi : mode=%s | dir=%s | size=%zu MB | bs=%zu KB | ops=%zu | fsync=%s | run=%d\n\n",
           cfg.mode_str, cfg.dir, cfg.size_mb, cfg.bs_kb, num_ops,
           (cfg.use_fsync && is_write_mode(cfg.mode)) ? "ya" : "tidak", cfg.run_id);

    io_print_metrics_snapshot("Sebelum",
                              has_mem_before ? &mem_before : NULL,
                              has_blk ? cfg.dev : NULL,
                              has_blk ? &blk_before : NULL);
    printf("\n");
    io_print_metrics_snapshot("Sesudah",
                              has_mem_after ? &mem_after : NULL,
                              has_blk ? cfg.dev : NULL,
                              has_blk ? &blk_after : NULL);

    if (has_blk)
    {
        io_blkstat_t blk_diff = io_diff_blkstat(&blk_before, &blk_after);
        io_print_blkstat_diff(cfg.dev, &blk_diff);
    }

    printf("\nWaktu eksekusi : %.3f s", elapsed_s);
    if (cfg.use_fsync && is_write_mode(cfg.mode))
    {
        printf(" (fsync: %.3f s)", (double)fsync_ns / 1e9);
    }
    printf("\n");
    printf("Throughput     : %.1f MB/s\n", throughput_mbs);
    io_print_stats(&stats);
    printf("Log            : %s\n", csv.path);
    if (env_path[0] != '\0')
    {
        printf("Env            : %s\n", env_path);
    }

    // Free the memory
    free(buf);
    free(latencies);
    return 0;
}
