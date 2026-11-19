import os
import sys
import re
import uuid
import subprocess
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# Executable name and path resolution (Windows .exe vs Linux ELF binary)
BINARY_NAME = "pcompress.exe" if sys.platform.startswith("win") else "pcompress"
BINARY_PATH = os.path.join(BASE_DIR, BINARY_NAME)

# Temporary directory for handling file uploads and decompression targets
TEMP_DIR = os.path.join(BASE_DIR, "temp_uploads")
os.makedirs(TEMP_DIR, exist_ok=True)
DOWNLOAD_CACHE = {}

# Automatic compiler: recompiles C binary if source is newer than existing binary
def ensure_binary():
    c_source = os.path.join(BASE_DIR, "parallel_compress.c")
    rebuild = False
    if not os.path.exists(BINARY_PATH):
        rebuild = True
    elif os.path.exists(c_source) and os.path.getmtime(c_source) > os.path.getmtime(BINARY_PATH):
        rebuild = True

    if rebuild and os.path.exists(c_source):
        print(f"[Build] Rebuilding {BINARY_NAME} from {c_source}...", flush=True)
        cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-std=c11", "-static", c_source, "-o", BINARY_PATH, "-lz"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[Build Error] Failed to compile {c_source}:\n{res.stderr}", flush=True)
        else:
            print(f"[Build] Successfully compiled {BINARY_PATH}!", flush=True)

# Threaded HTTP server handling concurrent web requests
class ThreadedServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

# HTTP Request Handler: routes web traffic and bridges to the C pcompress executable
class RequestHandler(BaseHTTPRequestHandler):
    # Helper to send JSON responses with appropriate headers
    def send_json(self, data, code=200):
        import json
        body = json.dumps(data).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # GET requests: Serve dashboard HTML, hardware CPU status, and file downloads
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        # Serve Dashboard Frontend (web/index.html)
        if path in ("/", "/index.html"):
            index_file = os.path.join(BASE_DIR, "web", "index.html")
            if os.path.exists(index_file):
                with open(index_file, "rb") as f:
                    content = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            else:
                self.send_error(404, "web/index.html not found")
            return

        # System Status API: returns available CPU cores and binary status
        if path == "/api/status":
            self.send_json({
                "cores": os.cpu_count() or 4,
                "os": sys.platform,
                "binary": os.path.basename(BINARY_PATH),
                "compiled": os.path.exists(BINARY_PATH)
            })
            return

        # File Download API: streams generated .pcmp or restored files to browser
        if path.startswith("/api/download/"):
            file_id = path.replace("/api/download/", "")
            if file_id in DOWNLOAD_CACHE:
                file_info = DOWNLOAD_CACHE[file_id]
                fpath = file_info["path"]
                fname = file_info["name"]
                if os.path.exists(fpath):
                    size = os.path.getsize(fpath)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Disposition", f'attachment; filename="{fname}"')
                    self.send_header("Content-Length", str(size))
                    self.end_headers()
                    with open(fpath, "rb") as f:
                        while chunk := f.read(65536):
                            self.wfile.write(chunk)
                    return
            self.send_error(404, "File not found")
            return

        self.send_error(404, "Not Found")

    # POST requests: Execute compression, decompression, or Amdahl's Law benchmark
    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        path = parsed.path

        content_len = int(self.headers.get("Content-Length", 0))
        raw_name = self.headers.get("X-Filename", "data.bin")
        filename = urllib.parse.unquote(raw_name)

        # Route 1: Parallel Compression API (/api/compress)
        if path == "/api/compress":
            threads = params.get("threads", [str(os.cpu_count() or 4)])[0]
            chunk_kib = params.get("chunk_kib", ["1024"])[0]
            level = params.get("level", ["6"])[0]

            job_id = str(uuid.uuid4())[:8]
            in_file = os.path.join(TEMP_DIR, f"{job_id}_{filename}")
            out_file = os.path.join(TEMP_DIR, f"{job_id}_{filename}.pcmp")

            # Stream incoming file body to temporary file on disk
            with open(in_file, "wb") as f:
                rem = content_len
                while rem > 0:
                    chunk = self.rfile.read(min(rem, 65536))
                    if not chunk: break
                    f.write(chunk)
                    rem -= len(chunk)

            # Invoke the C pcompress binary with multithreading parameters
            cmd = [BINARY_PATH, "-c", in_file, out_file, str(threads), str(chunk_kib), str(level)]
            proc = subprocess.run(cmd, capture_output=True, text=True)

            log_output = proc.stdout + "\n" + proc.stderr
            if proc.returncode != 0:
                try: os.remove(in_file)
                except Exception: pass
                self.send_json({"error": log_output or "Compression failed"}, code=400)
                return

            # Compute compression ratio and storage saved using actual byte counts
            orig_size = content_len
            comp_size = os.path.getsize(out_file) if os.path.exists(out_file) else 0
            ratio_pct = round((comp_size / orig_size) * 100, 2) if orig_size > 0 else 100.0
            saved_pct = round(((orig_size - comp_size) / orig_size) * 100, 2) if orig_size > 0 else 0.0

            # Extract execution time and throughput from C program standard output
            time_match = re.search(r"Execution Time\s*:\s*([\d\.]+)", log_output)
            tput_match = re.search(r"Throughput\s*:\s*([\d\.]+)", log_output)
            elapsed = float(time_match.group(1)) if time_match else 0.0
            throughput = float(tput_match.group(1)) if tput_match else 0.0

            # Cache output file path for browser download
            DOWNLOAD_CACHE[job_id] = {
                "path": out_file,
                "name": f"{filename}.pcmp"
            }
            try: os.remove(in_file)
            except Exception: pass

            self.send_json({
                "success": True,
                "job_id": job_id,
                "filename": f"{filename}.pcmp",
                "original_size": orig_size,
                "compressed_size": comp_size,
                "compression_ratio": ratio_pct,
                "saved_percent": saved_pct,
                "elapsed_seconds": elapsed,
                "throughput_mb_s": throughput,
                "threads": int(threads),
                "level": int(level),
                "terminal_log": log_output.strip()
            })
            return

        # Route 2: Parallel Decompression API (/api/decompress)
        if path == "/api/decompress":
            threads = params.get("threads", [str(os.cpu_count() or 4)])[0]
            job_id = str(uuid.uuid4())[:8]

            in_file = os.path.join(TEMP_DIR, f"{job_id}_{filename}")
            restored_name = filename[:-5] if filename.lower().endswith(".pcmp") else f"restored_{filename}"
            out_file = os.path.join(TEMP_DIR, f"{job_id}_{restored_name}")

            # Save uploaded .pcmp archive to disk
            with open(in_file, "wb") as f:
                rem = content_len
                while rem > 0:
                    chunk = self.rfile.read(min(rem, 65536))
                    if not chunk: break
                    f.write(chunk)
                    rem -= len(chunk)

            # Invoke C decompressor with multi-worker threads
            cmd = [BINARY_PATH, "-d", in_file, out_file, str(threads)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            log_output = proc.stdout + "\n" + proc.stderr

            if proc.returncode != 0:
                try: os.remove(in_file)
                except Exception: pass
                self.send_json({"error": log_output or "Decompression failed"}, code=400)
                return

            restored_size = os.path.getsize(out_file) if os.path.exists(out_file) else 0
            time_match = re.search(r"Execution Time\s*:\s*([\d\.]+)", log_output)
            tput_match = re.search(r"Throughput\s*:\s*([\d\.]+)", log_output)
            ext_match = re.search(r"Original Extension:\s*(\.[a-zA-Z0-9_\-]+)", log_output)
            elapsed = float(time_match.group(1)) if time_match else 0.0
            throughput = float(tput_match.group(1)) if tput_match else 0.0

            # Restore original file extension saved in archive header
            if ext_match and not restored_name.endswith(ext_match.group(1)):
                restored_name += ext_match.group(1)

            # Cache restored file for user download
            DOWNLOAD_CACHE[job_id] = {
                "path": out_file,
                "name": restored_name
            }
            try: os.remove(in_file)
            except Exception: pass

            self.send_json({
                "success": True,
                "job_id": job_id,
                "filename": restored_name,
                "restored_size": restored_size,
                "elapsed_seconds": elapsed,
                "throughput_mb_s": throughput,
                "threads": int(threads),
                "terminal_log": log_output.strip()
            })
            return

        # Route 3: Amdahl's Law Benchmark API (/api/benchmark)
        if path == "/api/benchmark":
            job_id = str(uuid.uuid4())[:8]
            if content_len > 0:
                bench_file = os.path.join(TEMP_DIR, f"bench_{job_id}_{filename}")
                with open(bench_file, "wb") as f:
                    rem = content_len
                    while rem > 0:
                        chunk = self.rfile.read(min(rem, 65536))
                        if not chunk: break
                        f.write(chunk)
                        rem -= len(chunk)
            else:
                bench_file = os.path.join(BASE_DIR, "parallel_compress.c")

            # Execute -b benchmark mode in C binary
            cmd = [BINARY_PATH, "-b", bench_file]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            log_output = proc.stdout

            if content_len > 0:
                try: os.remove(bench_file)
                except Exception: pass

            # Parse thread scalability results: thread count, duration, throughput, speedup
            results = []
            for line in log_output.splitlines():
                parts = line.strip().split()
                if len(parts) >= 4 and parts[0].isdigit() and ("." in parts[1]):
                    try:
                        th = int(parts[0])
                        sec = float(parts[1])
                        tput = float(parts[2])
                        speedup = float(parts[3].replace("x", ""))
                        results.append({
                            "threads": th,
                            "time": sec,
                            "throughput": tput,
                            "speedup": speedup
                        })
                    except ValueError:
                        pass

            self.send_json({
                "results": results,
                "raw_log": log_output.strip()
            })
            return

        self.send_error(404)

# Server startup entry point
def main():
    ensure_binary()
    port = 5050
    server = ThreadedServer(("127.0.0.1", port), RequestHandler)
    print("=" * 65)
    print(f" OS Project Dashboard Server running at: http://localhost:{port}")
    print(f" Using binary: {BINARY_PATH}")
    print(" Press Ctrl+C in this terminal to terminate.")
    print("=" * 65)
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server.")
        server.server_close()

if __name__ == "__main__":
    main()
