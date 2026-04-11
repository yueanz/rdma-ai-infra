#include "bench_utils.h"
#include "collective.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>

struct Config
{
    int rank      = 0;
    int world     = 0;
    int base_port = 12345;
    std::vector<std::string> host_list;
    int count     = 1024;
    int iters     = 100;
    bool is_rdma  = false;
};

static void config_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <rank> <size> <base_port> <host0> <host1> ... [--iters n] [--count n] [--rdma]\n", prog);
}

static int config_parse(int argc, char *argv[], Config *cfg) {
    if (argc < 4) {
        config_usage(argv[0]);
        return -1;
    }
    
    cfg->rank = atoi(argv[1]);
    cfg->world = atoi(argv[2]);
    cfg->base_port = atoi(argv[3]);

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
        }
    }
    return 0;
}

int main(int argc, char *argv[]){
    Config cfg;

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
    
    std::vector<float> buf(cfg.count);
    for (size_t i = 0; i < buf.size(); i++) {
        buf[i] = cfg.rank + 1;
    }

    if (ring_allreduce(&w, buf.data(), cfg.count) != 0) {
        LOG_ERR("ring_allreduce failed");
        return -1;
    }

    float expect = (1 + cfg.world) * cfg.world / 2.0f;
    for (size_t i = 0; i < buf.size(); i++) {
        if (std::abs(buf[i] - expect) > 1e-3) {
            LOG_ERR("ring_allreduce failed: correctness check failed idx: %zu", i);
            return -1;
        }
    }

    if (cfg.rank == 0)
        LOG_INFO("correctness check passed (expect=%.1f)", expect);
 
    uint64_t start;
    std::vector<uint64_t> latencies(cfg.iters);
    int total_iters = kWarmup + cfg.iters;
    for (int i = 0; i < total_iters; i++) {
        // reset buf before each iter
        for (size_t i = 0; i < buf.size(); i++)
            buf[i] = cfg.rank + 1;
        start = time_now_ns();
        if (ring_allreduce(&w, buf.data(), cfg.count) != 0) {
            LOG_ERR("ring_allreduce failed");
            return -1;
        }
        if (i >= kWarmup)
            latencies[i - kWarmup] = time_elapsed_ns(start, time_now_ns());
    }

    if (cfg.rank == 0) {
        std::sort(latencies.begin(), latencies.end());
        print_latency(cfg.is_rdma ? "ring_allreduce (rdma)" : "ring_allreduce (tcp)", latencies.data(), cfg.iters);
    }

    return 0;
}