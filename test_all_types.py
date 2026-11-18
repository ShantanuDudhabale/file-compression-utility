import os
import sys
import hashlib
import subprocess
import re
import zipfile
import io

def sha256_file(filepath):
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def create_valid_docx(filepath):
    content_types = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'
    rels = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'
    doc_xml = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Operating Systems Parallel Compression Course Project - Sample DOCX Document with formatting and content.</w:t></w:r></w:p></w:body></w:document>' * 5000

    with zipfile.ZipFile(filepath, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", doc_xml)

def create_valid_pdf(filepath):
    content = b"""%PDF-1.4
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>
endobj
4 0 obj
<< /Length 120 >>
stream
BT
/F1 18 Tf
50 700 Td
(Operating Systems Project: Parallel File Compression Test PDF Document) Tj
ET
endstream
endobj
5 0 obj
<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>
endobj
xref
0 6
0000000000 65535 f 
0000000010 00000 n 
0000000060 00000 n 
0000000117 00000 n 
0000000241 00000 n 
0000000413 00000 n 
trailer
<< /Size 6 /Root 1 0 R >>
startxref
484
%%EOF
"""
    with open(filepath, "wb") as f:
        f.write(content)
        comment_pad = (b"% Operating Systems Project PDF Padding Stream " * 20 + b"\n") * 1000
        f.write(comment_pad)

def create_valid_png(filepath):
    import zlib, struct
    width, height = 200, 200
    rows = []
    for y in range(height):
        row = bytearray([0]) # Filter type None
        for x in range(width):
            row.extend([(x + y) % 256, (x * 2) % 256, (y * 2) % 256])
        rows.append(bytes(row))
    raw_data = b"".join(rows)
    compressed = zlib.compress(raw_data)
    
    png = b"\x89PNG\r\n\x1a\n"
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    ihdr_crc = struct.pack(">I", zlib.crc32(b"IHDR" + ihdr_data))
    png += struct.pack(">I", len(ihdr_data)) + b"IHDR" + ihdr_data + ihdr_crc
    idat_crc = struct.pack(">I", zlib.crc32(b"IDAT" + compressed))
    png += struct.pack(">I", len(compressed)) + b"IDAT" + compressed + idat_crc
    iend_crc = struct.pack(">I", zlib.crc32(b"IEND"))
    png += struct.pack(">I", 0) + b"IEND" + iend_crc

    with open(filepath, "wb") as f:
        f.write(png)

def create_valid_jpg(filepath):
    jfif_header = bytes([
        0xFF, 0xD8,                                     # SOI
        0xFF, 0xE0, 0x00, 0x10,                         # APP0
        0x4A, 0x46, 0x49, 0x46, 0x00,                   # "JFIF\0"
        0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0xFF, 0xDB, 0x00, 0x43, 0x00                   # DQT
    ]) + bytes([16] * 64) + bytes([
        0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x40, 0x00, 0x40, 0x01, 0x01, 0x11, 0x00, # SOF0 (64x64)
        0xFF, 0xC4, 0x00, 0x1F, 0x00,                   # DHT
    ]) + bytes([0] * 16) + bytes([1] * 12) + bytes([
        0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 # SOS
    ]) + bytes([0xAA, 0x55] * 20000) + bytes([
        0xFF, 0xD9                                      # EOI
    ])
    with open(filepath, "wb") as f:
        f.write(jfif_header)

def main():
    print("=" * 80, flush=True)
    print("   AUTOMATED VERIFICATION TEST: COMPRESSION & DECOMPRESSION ON 6 FILE TYPES", flush=True)
    print("=" * 80, flush=True)

    test_files = [
        ("repetitive_text.txt", "1. Repetitive TXT (10 MB)"),
        ("document.docx",       "2. DOCX Document (ZIP-based)"),
        ("sample.pdf",          "3. PDF Document"),
        ("image.jpg",           "4. JPG Image (Lossy compressed)"),
        ("image.png",           "5. PNG Image (Deflate compressed)"),
        ("random_binary.bin",   "6. Random Binary (High entropy)")
    ]

    print("\n[Step 1] Generating test files...", flush=True)
    
    pattern = "Operating Systems Project\nParallel File Compression\nMultithreading\n"
    repeat_count = (10 * 1024 * 1024) // len(pattern)
    with open("repetitive_text.txt", "w", encoding="utf-8") as f:
        f.write(pattern * repeat_count)
    
    create_valid_docx("document.docx")
    
    create_valid_pdf("sample.pdf")
    
    create_valid_jpg("image.jpg")
    
    create_valid_png("image.png")
    
    with open("random_binary.bin", "wb") as f:
        f.write(os.urandom(1024 * 1024))

    print("All test files generated successfully.\n", flush=True)

    binary = os.path.abspath("pcompress.exe")
    assert os.path.exists(binary), f"pcompress.exe not found at {binary}"

    results = []
    print("[Step 2] Executing Compression -> SHA-256 Check -> Decompression -> SHA-256 Verification\n", flush=True)

    for fname, label in test_files:
        orig_path = fname
        pcmp_path = fname + ".pcmp"
        restored_path = "restored_" + fname

        orig_size = os.path.getsize(orig_path)
        orig_sha = sha256_file(orig_path)

        comp_cmd = [binary, "-c", orig_path, pcmp_path, "4", "1024", "6"]
        comp_proc = subprocess.run(comp_cmd, capture_output=True, text=True)

        if comp_proc.returncode != 0:
            print(f"[FAIL] Compression failed for {fname}:\n{comp_proc.stderr}", flush=True)
            continue

        comp_size = os.path.getsize(pcmp_path)
        time_m = re.search(r"Execution Time\s*:\s*([\d\.]+)", comp_proc.stdout)
        tput_m = re.search(r"Throughput\s*:\s*([\d\.]+)", comp_proc.stdout)
        elapsed = float(time_m.group(1)) if time_m else 0.0
        tput = float(tput_m.group(1)) if tput_m else 0.0

        ratio = round(100.0 * comp_size / orig_size, 2) if orig_size > 0 else 100.0
        saved = round(100.0 - ratio, 2)

        decomp_cmd = [binary, "-d", pcmp_path, restored_path, "4"]
        decomp_proc = subprocess.run(decomp_cmd, capture_output=True, text=True)

        if decomp_proc.returncode != 0:
            print(f"[FAIL] Decompression failed for {fname}:\n{decomp_proc.stderr}", flush=True)
            continue

        restored_size = os.path.getsize(restored_path)
        restored_sha = sha256_file(restored_path)

        is_byte_exact = (orig_sha == restored_sha) and (orig_size == restored_size)

        results.append({
            "label": label,
            "filename": fname,
            "orig_size": orig_size,
            "comp_size": comp_size,
            "ratio": ratio,
            "saved": saved,
            "time": elapsed,
            "tput": tput,
            "orig_sha": orig_sha[:12] + "...",
            "restored_sha": restored_sha[:12] + "...",
            "exact_match": is_byte_exact
        })

        os.remove(orig_path)
        os.remove(pcmp_path)
        os.remove(restored_path)

    print("=" * 115, flush=True)
    print(f"{'File Type':<32} {'Orig Size':<14} {'Comp Size':<14} {'Ratio (%)':<11} {'Saved (%)':<11} {'Time (s)':<10} {'SHA-256 Match'}", flush=True)
    print("=" * 115, flush=True)
    for r in results:
        match_str = "[PASS] IDENTICAL" if r["exact_match"] else "[FAIL] MISMATCH"
        print(f"{r['label']:<32} {r['orig_size']:<14,} {r['comp_size']:<14,} {r['ratio']:>7.2f}% {r['saved']:>8.2f}% {r['time']:>8.4f}s  {match_str}", flush=True)
    print("=" * 115, flush=True)

    print("\n[Step 3] Multithreading Scalability Test (1, 2, 4, 8 Threads on 10 MB Repeated Text)\n", flush=True)
    with open("repetitive_10mb.txt", "w", encoding="utf-8") as f:
        f.write(pattern * repeat_count)
    
    m_orig_size = os.path.getsize("repetitive_10mb.txt")
    m_results = []
    for th in [1, 2, 4, 8]:
        out = f"m_{th}.pcmp"
        proc = subprocess.run([binary, "-c", "repetitive_10mb.txt", out, str(th), "1024", "6"], capture_output=True, text=True)
        csz = os.path.getsize(out)
        time_m = re.search(r"Execution Time\s*:\s*([\d\.]+)", proc.stdout)
        tput_m = re.search(r"Throughput\s*:\s*([\d\.]+)", proc.stdout)
        sec = float(time_m.group(1)) if time_m else 0.0
        tput = float(tput_m.group(1)) if tput_m else 0.0
        r_pct = round(100.0 * csz / m_orig_size, 2)
        s_pct = round(100.0 - r_pct, 2)
        m_results.append({
            "threads": th,
            "orig_size": m_orig_size,
            "comp_size": csz,
            "ratio": r_pct,
            "saved": s_pct,
            "time": sec,
            "tput": tput,
            "level": 6
        })
        os.remove(out)
    os.remove("repetitive_10mb.txt")

    print("=" * 105, flush=True)
    print(f"{'Threads':<10} {'Original Size':<16} {'Compressed Size':<18} {'Ratio (%)':<11} {'Saved (%)':<11} {'Time (s)':<11} {'Throughput'}", flush=True)
    print("=" * 105, flush=True)
    for mr in m_results:
        print(f"{mr['threads']:<10} {mr['orig_size']:<16,} {mr['comp_size']:<18,} {mr['ratio']:>7.2f}% {mr['saved']:>8.2f}% {mr['time']:>8.4f}s {mr['tput']:>8.1f} MB/s", flush=True)
    print("=" * 105, flush=True)

if __name__ == "__main__":
    main()
