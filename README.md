# SIM Library v1.4.0

Ultra-low latency shared memory transport for robotics and autonomous systems.

[![License: MIT with Citation](https://img.shields.io/badge/License-MIT_with_Citation-blue.svg)](LICENSE)

---

## Data Disclaimer

> **IMPORTANT:** Benchmark results may contain measurement errors. Data was collected using custom scripts with embedded timestamps—**FastDDS Monitor was NOT used** for the latest tests. Results should be considered indicative. Hardware variations and system load affect actual performance.

**Methodology:** End-to-end latency from `high_resolution_clock`, 12-15s tests at 30Hz, median values.

## Transport Family Overview

All transports are based on **SIM (Sensor-In-Memory)** architecture with different optimizations:

| Transport | Full Name | Architecture |
|-----------|-----------|--------------|
| **BARQ** | Burst Access Reader Queue | Double buffer + NT stores |
| **CASIR** | Cache Access Streaming Into Reader | Double buffer + prefetch |
| **SAHM** | Sensor Acquisition to Host Memory | Ring buffer × N readers |
| **SIM** | Sensor-In-Memory | Double buffer (basic) |

---

## Performance Summary (Benchmark V2 - December 2025)

### Single Subscriber (Median Latency in ms)

| Transport | 1KB | 10KB | 100KB | 1MB | 5MB | 10MB | 20MB | 50MB |
|-----------|-----|------|-------|-----|-----|------|------|------|
| **CASIR** | 0.0008 | 0.0010 | 0.0059 | 0.067 | 0.53 | 1.04 | 2.17 | **4.49** |
| **BARQ** | 0.0007 | 0.0012 | 0.0064 | 0.059 | 0.54 | 1.08 | 2.26 | 5.19 |
| **SIM** | 0.0010 | 0.0033 | 0.0160 | 0.136 | 0.82 | 1.65 | 3.58 | 9.45 |
| ROS2 Std | 0.61 | 0.63 | 0.73 | 1.56 | 4.76 | 8.78 | 17.7 | 360 |
| ROS2 Loan | 0.59 | 0.61 | 0.72 | 1.34 | 4.93 | 9.26 | 16.2 | 544 |

**50MB: CASIR 80-120x faster than ROS2!**

---

## Winner by Data Size

| Data Size | Winner | Latency | vs ROS2 |
|-----------|-----------|---------|---------|
| **1-10 KB** | **CASIR** | 0.8-1.0 μs | ~600x faster |
| **100 KB** | **CASIR/BARQ** | 6 μs | ~120x faster |
| **1 MB** | **BARQ** | 59 μs | ~23x faster |
| **5-20 MB** | **CASIR** | 0.5-2.2 ms | ~8x faster |
| **50 MB** | **CASIR** | 4.5 ms | ~80-120x faster |

---

## Multi-Subscriber Scaling (1MB)

| Transport | 1 Sub | 2 Subs | 3 Subs | 5 Subs |
|-----------|-------|--------|--------|--------|
| **BARQ** | 0.059 | 0.057 | 0.058 | 0.060 |
| **CASIR** | 0.067 | 0.070 | 0.067 | 0.067 |
| **SIM** | 0.136 | 0.143 | 0.157 | 0.164 |
| ROS2 Standard | 1.56 | 1.35 | 1.47 | 1.59 |
| ROS2 Loaned | 1.34 | 1.41 | 1.44 | 1.60 |

**SIM transports: stable latency even with 5 subscribers**

---

## Unified API (All Transports)

All SIM transports share the same simple API pattern:

```cpp
// Writer (same pattern for all transports)
namespace NAME {
  class Writer {
    Writer(channel_name, max_size, config = default);
    bool init();
    bool write(data, size);
    void* getWriteBuffer();    // For zero-copy
    bool commit(size);         // After zero-copy fill
    void destroy();
  };
}

// Reader (same pattern for all transports)
namespace NAME {
  class Reader {
    Reader(channel_name, max_size, config = default);
    bool init();
    const void* read(size&);   // Zero-copy
    bool isWriterAlive(timeout_ms);
  };
}
```

---

## Quick Start Examples

### BARQ (Burst Access Reader Queue) - Fastest Overall

```cpp
#include "barq.hpp"

// Writer
BARQ::Writer writer("/sensor", max_size, true);  // use_huge_pages
writer.init();
writer.write(data, size);

// Reader (true zero-copy)
BARQ::Reader reader("/sensor", max_size);
reader.init();
size_t sz; int64_t ts;
const void* ptr = reader.getLatest(sz, ts);
```

### CASIR (Cache Access Streaming Into Reader) - Best for Small Data

```cpp
#include "casir.hpp"

auto cfg = CASIR::Config::autoDetect();

// Writer
CASIR::Writer writer("/sensor", max_size, cfg);
writer.init();
writer.write(data, size);

// Reader
CASIR::Reader reader("/sensor", max_size, cfg);
reader.init();
size_t sz;
const void* ptr = reader.readZeroCopy(sz);
```

### SAHM (Sensor Acquisition to Host Memory) - Multi-Reader

```cpp
#include "sahm.hpp"

// Writer (broadcasts to all readers)
SAHM::DirectWriter writer("/sensor", max_size);
writer.init();
writer.write(data, size);

// Reader (30-slot ring buffer with history)
SAHM::DirectReader reader("/sensor", max_size, 30);
reader.init();
size_t sz;
const void* ptr = reader.getLatest(sz);
```

### SIM (Sensor-In-Memory) - Simplest

```cpp
#include "sim_transport.hpp"

// Writer
SIM::Writer writer("/sensor", max_size);
writer.init();
writer.write(data, size);

// Reader
SIM::Reader reader("/sensor", max_size);
reader.init();
size_t sz;
const void* ptr = reader.readZeroCopy(sz);
```

---

## Architecture Details

### BARQ (Burst Access Reader Queue)
```
┌────────────────────────────────────────────────┐
│  Header (5 × 64B = 320B cache-aligned)         │
│  ┌──────────────────────────────────────────┐  │
│  │ CL0: Magic | Version | Capacity | Flags  │  │
│  │ CL1: atomic<front_idx> ← 1 atomic/write  │  │
│  │ CL2-3: Buffer metadata (seq, ts, len)    │  │
│  │ CL4: Heartbeat | Stats                   │  │
│  └──────────────────────────────────────────┘  │
├────────────────────────────────────────────────┤
│  Buffer A ← Non-temporal stores (bypass cache) │
├────────────────────────────────────────────────┤
│  Buffer B ← Reader gets direct SHM pointer     │
└────────────────────────────────────────────────┘
```

### CASIR (Cache Access Streaming Into Reader)
```
┌────────────────────────────────────────────────┐
│  Header (6 × 64B = 384B)                       │
│  - Each atomic on separate cache line          │
│  - Auto-detect: huge pages, NUMA, prefetch     │
├────────────────────────────────────────────────┤
│  Buffer A ← Software prefetch ahead            │
├────────────────────────────────────────────────┤
│  Buffer B ← Software prefetch ahead            │
└────────────────────────────────────────────────┘
```

### SAHM (Sensor Acquisition to Host Memory)
```
┌─────────── CONTROL CHANNEL ─────────────┐
│  [Magic] [Reader Registry 0..7]         │
└──────────────────┬──────────────────────┘
    ┌──────────────┼──────────────┐
    ▼              ▼              ▼
┌─────────┐  ┌─────────┐   ┌─────────┐
│Reader 1 │  │Reader 2 │   │Reader N │
│Ring[30] │  │Ring[30] │   │Ring[30] │
└─────────┘  └─────────┘   └─────────┘
```

---

## When to Use

| Scenario | Recommendation |
|----------|----------------|
| **Minimum latency** | BARQ |
| **Small data (≤10KB)** | CASIR |
| **Large data (≥20MB)** | BARQ |
| **Medium data (5-10MB)** | SAHM |
| **Multiple readers** | SAHM |
| **History/replay** | SAHM |
| **Simplest API** | SIM |

---

## Files

```
Library_release/
├── include/
│   ├── sim.hpp            # SIM - Simple
│   ├── sahm.hpp           # SAHM - Multi-reader
│   ├── barq.hpp           # BARQ - Fastest
│   ├── casir.hpp          # CASIR - Cache-optimized
│   └── cache_utils.hpp    # CASIR dependency
├── src/
│   ├── sim.cpp
│   ├── sahm.cpp
│   ├── barq.cpp
│   ├── casir.cpp
│   └── cache_utils.cpp
├── examples/
│   ├── simple_writer.cpp  # SIM
│   ├── simple_reader.cpp  # SIM
│   ├── sahm_writer.cpp    # SAHM
│   ├── sahm_reader.cpp    # SAHM
│   ├── turbo_writer.cpp   # CASIR
│   └── turbo_reader.cpp   # CASIR
├── docs/
├── CMakeLists.txt.example
├── CHANGELOG.md
├── ACKNOWLEDGMENTS.md
├── LICENSE
└── README.md
```

---

## Requirements

- C++17 (GCC 7+, Clang 5+)
- Linux (POSIX shared memory)
- SSE2 for non-temporal stores (BARQ)

---

## Credits

Based on: **"A Faster and More Reliable Middleware for Autonomous Driving Systems"**  
Yuankai He, Weisong Shi - University of Delaware

---

**Author:** Nabil Achir | **Version:** 1.4.0 | **December 2025**
