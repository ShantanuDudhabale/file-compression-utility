# Operating Systems Project: Parallel File Compressor (pthreads)

A lightweight, multi-threaded file compression and decompression utility written in **C (C11)** using **POSIX Threads (`pthreads`)** and the **zlib (DEFLATE)** compression library.

---

## 1. Project Overview & Motivation

Standard compression utilities (like basic gzip) are single-threaded and can only utilize 1 CPU core at a time, leaving modern multi-core processors largely underutilized. 

This project demonstrates **Data Parallelism** at the operating system level:
- Large files are divided into independent **1 MB chunks**.
- Worker threads (`pthread_create`) compress these chunks concurrently across all available CPU cores.
- A coordinator thread waits for all threads to finish (`pthread_join`) and writes the compressed chunks to disk in strict sequential order.
- Decompression restores the exact file with per-chunk **CRC32 checksum** verification.

---

## 2. Operating System Concepts Covered

| OS Concept | How It Is Implemented in This Project |
| :--- | :--- |
| **POSIX Threading** | Uses `pthread_create()` to spawn workers and `pthread_join()` for barrier synchronization. |
| **Data Parallelism** | Splits file into chunks; each thread processes its own chunk independently. |
| **Lock-Free Concurrency** | Each thread operates strictly on isolated memory buffers, eliminating mutex lock overhead and lock contention. |
| **Race Condition Avoidance** | Worker threads never write directly to the output file; only the coordinator thread writes in deterministic order. |
| **Amdahl's Law** | Benchmark mode (`-b`) demonstrates diminishing speedup returns as disk I/O becomes the bottleneck. |

---

## 3. Project Structure

```
.
├── parallel_compress.c     # Complete C source code for the compressor & decompressor
├── Makefile                # Build script for Linux / Ubuntu and Windows
├── server.py               # Lightweight Python server hosting the web interface
├── web/
│   └── index.html          # Clean web dashboard with live thread terminal & benchmark
├── run_demo.bat            # One-click demonstration script for Windows
├── start_dashboard.bat     # One-click web interface launcher
└── README.md               # Project documentation
```

---

## 4. How to Compile

### On Linux / Ubuntu
```bash
# Install dependencies (if not already installed)
sudo apt update
sudo apt install build-essential zlib1g-dev

# Compile using Makefile
make

# Or compile directly with GCC
gcc -O2 -Wall -Wextra -std=c11 parallel_compress.c -o pcompress -lz -pthread
```

### On Windows (MinGW / MSYS2)
```powershell
gcc -O2 -Wall -Wextra -std=c11 -static parallel_compress.c -o pcompress.exe -lz
```

---

## 5. How to Run

### Command-Line Usage

```bash
# 1. Compress a file (threads, chunk_kib, and level are optional)
./pcompress -c <input_file> <output.pcmp> [threads] [chunk_kib] [level]

# Example (4 threads, 1024 KB chunk, Level 9 Maximum):
./pcompress -c my_document.pdf compressed.pcmp 4 1024 9

# 2. Decompress a file
./pcompress -d <input.pcmp> <restored_file> [threads]

# Example:
./pcompress -d compressed.pcmp restored.pdf 4

# 3. Run Concurrency Benchmark (Amdahl's Law analysis across 1, 2, 4, 8, N threads)
./pcompress -b <input_file>
```

### Web Dashboard
Double-click `start_dashboard.bat` (Windows) or run:
```bash
python server.py
```
Open your browser at **`http://localhost:5050`**.
- Drag-and-drop any file to compress or decompress.
- Adjust thread count slider in real time.
- View live **OS Thread Scheduler Logs** in the on-screen terminal.
- View interactive **Amdahl's Law Scaling Charts**.
- Review built-in **OS Viva Questions & Explanations**.

---

## 6. Binary Archive Format (`.pcmp`)

The custom binary format is structured as follows:

```
+-------------------------------------------------------------+
| Header (36 bytes):                                          |
|   - Magic Bytes: "PCMP" (4 bytes)                           |
|   - Original File Size: uint64_t (8 bytes)                  |
|   - Total Chunks: uint32_t (4 bytes)                        |
|   - Chunk Size: uint32_t (4 bytes)                          |
|   - Original Extension: char[16] (16 bytes, e.g. ".docx")   |
+-------------------------------------------------------------+
| Chunk 0:                                                    |
|   - Original Chunk Length: uint32_t (4 bytes)               |
|   - Compressed Chunk Length: uint32_t (4 bytes)             |
|   - CRC32 Checksum: uint32_t (4 bytes)                      |
|   - Compressed Payload Bytes                                |
+-------------------------------------------------------------+
| Chunk 1 ... Chunk N (same structure as Chunk 0)             |
+-------------------------------------------------------------+
```

---

## 7. Experimental Results: Thread Scaling vs File Size

Testing performed on a **~10.45 MB repeated text sample** across varying thread configurations:

| Threads | Original Size | Compressed Size | Space Saved | Execution Time | Throughput | Speedup |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1 Thread** | 10,955,210 B | 38,319 B | **99.65%** | 0.0650 s | 168.2 MB/s | 1.00x |
| **2 Threads** | 10,955,210 B | 38,319 B | **99.65%** | 0.0360 s | 302.7 MB/s | **1.81x** |
| **4 Threads** | 10,955,210 B | 38,319 B | **99.65%** | 0.0290 s | 377.3 MB/s | **2.24x** |
| **8 Threads** | 10,955,210 B | 38,319 B | **99.65%** | 0.0200 s | 560.2 MB/s | **3.25x** |

### Key Observations for OS Viva:
1. **Identical Output Size**: Because the algorithm (DEFLATE), level, and chunk size (1 MiB) are identical, the compressed output size is identical (`38,319` bytes) regardless of thread count.
2. **Speedup**: Increasing threads from 1 to 8 reduced execution time from **0.065s down to 0.020s** (throughput jumped from 168 MB/s to 560 MB/s), proving that multi-threading delivers **processing speedup**, not size reduction.


