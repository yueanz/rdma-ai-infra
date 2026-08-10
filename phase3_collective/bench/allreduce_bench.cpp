#include "bench_utils.h"
#include "collective.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <cstdlib>

/* Page-aligned float array. A std::vector lands at an arbitrary offset and
 * usually straddles a page boundary, which phase 2 measured at ~0.5 us of
 * round trip on 4 KB. That was not re-measured here, and the effect should be
 * smaller: straddling costs the same once per transfer whatever the size, and
 * nothing here is smaller than 64 KB. Kept because alignment is free. */
struct AlignedFloats {
    float *p = nullptr;
    int init(size_t n) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) ps = 4096;
        void *raw = nullptr;
        if (posix_memalign(&raw, (size_t)ps, n * sizeof(float)) != 0)
            return -1;
        p = (float*)raw;
        return 0;
    }
    ~AlignedFloats() { free(p); }
    AlignedFloats() = default;
    AlignedFloats(const AlignedFloats&) = delete;
    AlignedFloats& operator=(const AlignedFloats&) = delete;
};

struct Config
{
    int rank      = 0;
    int world     = 0;
    int base_port = 12345;
    std::vector<std::string> host_list;
    int count     = 1024;
    int iters     = 100;
    bool is_rdma  = false;
    bool csv        = false;  /* one row on rank 0, nothing else on stdout */
    bool csv_header = false;  /* print the header and exit */
};

static void config_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <rank> <size> <base_port> <host0> <host1> ... [--iters n] [--count n] [--rdma] [--csv]\n", prog);
    fprintf(stderr, "  %s --csv-header\n", prog);
}

static int config_parse(int argc, char *argv[], Config *cfg) {
    if (argc < 4) {
        config_usage(argv[0]);
        return -1;
    }
    
    cfg->rank = atoi(argv[1]);
    cfg->world = atoi(argv[2]);
    cfg->base_port = atoi(argv[3]);

    if (cfg->world <= 0 || cfg->rank < 0 || cfg->rank >= cfg->world) {
        fprintf(stderr, "world must be > 0 and 0 <= rank < world\n");
        return -1;
    }

    for (int i = 0; i < cfg->world; i++) {
        if (4 + i >= argc) {
            config_usage(argv[0]);
            return -1;
        }
        cfg->host_list.push_back(argv[4 + i]);
    }
    for (int i = 4 + cfg->world; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value after --iters\n");
                return -1;
            }
            cfg->iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--count") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value after --count\n");
                return -1;
            }
            cfg->count = atoi(argv[++i]);  /* number of float32 elements in the allreduce buffer */
        } else if (strcmp(argv[i], "--rdma") == 0) {
            cfg->is_rdma = true;
        } else if (strcmp(argv[i], "--csv") == 0) {
            cfg->csv = true;
        }
    }
    if (cfg->count <= 0 || cfg->count % cfg->world != 0) {
        fprintf(stderr, "count must be > 0 and divisible by world (got count=%d world=%d)\n",
                cfg->count, cfg->world);
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]){
    Config cfg;

    /* Checked before config_parse, which insists on the positional args. */
    if (argc == 2 && strcmp(argv[1], "--csv-header") == 0) {
        printf("bench,conn,count,bytes,world,iters,min_us,median_us,p99_us,max_us,busbw_gbps\n");
        return 0;
    }

    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        return 1;
    }

    std::vector<const char*> hosts;
    for (auto &s : cfg.host_list)
        hosts.push_back(s.c_str());

    World w;
    if (world_init(&w, cfg.rank, cfg.world, hosts.data(), cfg.base_port, cfg.is_rdma) != 0) {
        LOG_ERR("world_init failed");
        return -1;
    }
    
    size_t chunk = cfg.count / w.size;
    size_t chunk_bytes = chunk * sizeof(float);

    /* stage holds two chunks: the RDMA path posts the next round's receive
     * into one half while the previous half is still being summed. */
    AlignedFloats buf, stage;
    if (buf.init(cfg.count) != 0 || stage.init(2 * chunk) != 0) {
        LOG_ERR("allreduce_bench: buffer allocation failed");
        return -1;
    }
    for (int i = 0; i < cfg.count; i++)
        buf.p[i] = cfg.rank + 1;

    // register buffers
    ScopedBuffer r_buf_h;
    if (r_buf_h.init(w.right.get(), buf.p, cfg.count * sizeof(float)) != 0) {
        LOG_ERR("ring_allreduce failed: sb init failed");
        return -1;
    }
    ScopedBuffer l_buf_h;
    if (l_buf_h.init(w.left.get(), buf.p, cfg.count * sizeof(float)) != 0) {
        LOG_ERR("ring_allreduce failed: sb init failed");
        return -1;
    }
    ScopedBuffer stage_h;
    if (stage_h.init(w.left.get(), stage.p, 2 * chunk_bytes) != 0) {
        LOG_ERR("ring_allreduce failed: sb init failed");
        return -1;
    }

    /* The barrier needs a byte to trade that is not part of the payload. */
    AlignedFloats scratch;
    if (scratch.init(16) != 0) {
        LOG_ERR("allreduce_bench: scratch allocation failed");
        return -1;
    }
    ScopedBuffer scratch_r, scratch_l;
    if (scratch_r.init(w.right.get(), scratch.p, 16 * sizeof(float)) != 0 ||
        scratch_l.init(w.left.get(), scratch.p, 16 * sizeof(float)) != 0) {
        LOG_ERR("allreduce_bench: scratch registration failed");
        return -1;
    }

    if (ring_allreduce(&w, &r_buf_h.h, &l_buf_h.h, &stage_h.h, buf.p, stage.p, cfg.count) != 0) {
        LOG_ERR("ring_allreduce failed");
        return -1;
    }

    float expect = (1 + cfg.world) * cfg.world / 2.0f;
    for (int i = 0; i < cfg.count; i++) {
        if (std::abs(buf.p[i] - expect) > 1e-3) {
            LOG_ERR("ring_allreduce failed: correctness check failed idx: %d", i);
            return -1;
        }
    }

    if (cfg.rank == 0 && !cfg.csv)
        LOG_INFO("correctness check passed (expect=%.1f)", expect);
 
    uint64_t iter_start;
    std::vector<uint64_t> latencies(cfg.iters);
    int total_iters = kWarmup + cfg.iters;
    for (int i = 0; i < total_iters; i++) {
        // reset buf before each iter
        for (int k = 0; k < cfg.count; k++)
            buf.p[k] = cfg.rank + 1;
        /* Line the ranks up before the clock starts: the reset above takes
         * longer on a bigger buffer and the two ranks do not finish it
         * together, which otherwise shows up as a stall inside the timed
         * region rather than as the skew it is. */
        if (world_barrier(&w, &scratch_r.h, &scratch_l.h) != 0) {
            LOG_ERR("world_barrier failed");
            return -1;
        }
        iter_start = time_now_ns();
        if (ring_allreduce(&w, &r_buf_h.h, &l_buf_h.h, &stage_h.h, buf.p, stage.p, cfg.count) != 0) {
            LOG_ERR("ring_allreduce failed");
            return -1;
        }
        if (i >= kWarmup)
            latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
    }

    /* Check again after the timed loop, not just before it. The barrier and
     * the buffer reset both touch memory the collective also uses, and a bug
     * there would otherwise never show: the pre-loop check runs before either
     * of them has executed once. */
    for (int i = 0; i < cfg.count; i++) {
        if (std::abs(buf.p[i] - expect) > 1e-3) {
            LOG_ERR("correctness check failed after timed loop at idx %d: %f != %f",
                    i, buf.p[i], expect);
            return -1;
        }
    }

    if (cfg.rank == 0) {
        std::sort(latencies.begin(), latencies.end());
        if (cfg.csv) {
            /* Bus bandwidth, the figure NCCL's own benchmarks report: a ring
             * all-reduce moves 2(N-1)/N times the buffer over each link, so
             * busbw stays flat across world sizes when the link is saturated
             * while a plain bytes/time figure would not. */
            double bytes  = (double)cfg.count * sizeof(float);
            double med_s  = latencies[cfg.iters/2] / 1e9;
            double algbw  = bytes / med_s;
            double busbw  = algbw * 2.0 * (w.size - 1) / w.size;
            printf("allreduce,%s,%d,%.0f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f\n",
                   cfg.is_rdma ? "rdma" : "tcp", cfg.count, bytes, w.size, cfg.iters,
                   latencies[0]/1000.0, latencies[cfg.iters/2]/1000.0,
                   latencies[(int)(cfg.iters*0.99)]/1000.0, latencies[cfg.iters-1]/1000.0,
                   busbw * 8.0 / 1e9);
        } else {
            print_latency(cfg.is_rdma ? "ring_allreduce (rdma)" : "ring_allreduce (tcp)", latencies.data(), cfg.iters);
        }
    }

    return 0;
}