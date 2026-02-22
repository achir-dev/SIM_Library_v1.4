# Changelog

All notable changes to SIM Library.

## [1.4.0] - 2025-12-14

### Added
- **Unified API pattern** across all transports for easier switching

### Changed
- Renamed **SAHM v2** → **BARQ** (Burst Access Reader Queue)
- Renamed **SiC** → **CASIR** (Cache Access Streaming Into Reader)
- Updated all documentation with full transport names:
  - SAHM = Sensor Acquisition to Host Memory
  - SIM = Sensor-In-Memory

---

## [1.3.0] - 2025-12-14

### Added
- **BARQ** (ex SAHM v2): Ultra-fast "shoot and forget" transport
  - Double buffer (no ring buffer overhead)
  - Non-temporal stores (SSE2 _mm_stream_si128)
  - Huge pages support (2MB, auto-fallback)
  - Cache-line aligned header (5 × 64B = 320B)
  - MAP_POPULATE + mlock for zero page faults
  - True zero-copy reader (direct SHM pointer)

---

## [1.2.0] - 2025-12-13

### Added
- **CASIR** (ex SiC): Cache-optimized transport
  - Huge pages support
  - Software prefetching
  - NUMA awareness
  - CPU affinity support

---

## [1.1.0] - 2025-12-12

### Added
- **SAHM** (Sensor Acquisition to Host Memory)
  - Ring buffer with multi-reader support
  - 30-slot ring buffer per reader
  - Independent reader SHM segments
  - History access for replay

---

## [1.0.0] - 2025-12-10

### Initial Release
- **SIM** (Sensor-In-Memory)
  - Double buffer lock-free transport
  - Zero-copy read support
  - Checksum validation (optional)

---

## Performance History

| Version | 5MB Latency | Key Improvement |
|---------|-------------|-----------------|
| 1.4.0 | ~0.35 ms | API unification |
| 1.3.0 | ~0.35 ms | BARQ non-temporal stores |
| 1.2.0 | ~0.40 ms | CASIR cache optimization |
| 1.1.0 | ~0.57 ms | SAHM ring buffer |
| 1.0.0 | ~0.83 ms | Initial SIM |
| ROS2 | ~5.0 ms | Baseline |

---

## Transport Overview

| Transport | Full Name | Best For |
|-----------|-----------|----------|
| **BARQ** | Burst Access Reader Queue | 20MB+ data |
| **CASIR** | Cache Access Streaming Into Reader | ≤10KB data |
| **SAHM** | Sensor Acquisition to Host Memory | Multi-reader |
| **SIM** | Sensor-In-Memory | Simple API |
