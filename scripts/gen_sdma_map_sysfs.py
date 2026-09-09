#!/usr/bin/env python3
# -------------------------------------------------------------------------------
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
# -------------------------------------------------------------------------------
"""
gen_sdma_map_sysfs.py — Generate sDMA engine affinity map from KFD sysfs.

Reads the recommended_sdma_engine_id_mask that the KFD kernel driver exposes
for each IO link and converts it to the CSV format expected by sdma_collective:

    src_gpu_index, engine_id, dst_gpu_index

Works on any GPU generation (MI300X, MI355X, …) without hardcoded tables or
root access — the kernel already knows the optimal engine per (src, dst) pair.

Usage:
    python3 gen_sdma_map_sysfs.py [--out <file>] [--verify <existing_csv>]
"""

import argparse, math, os, sys

TOPO_ROOT = "/sys/devices/virtual/kfd/kfd/topology/nodes"
XGMI_LINK_TYPE = 11


def read_node_property(node_id, key):
    with open(os.path.join(TOPO_ROOT, str(node_id), "properties")) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[0] == key:
                return int(parts[1])
    return None


def read_link_properties(node_id, link_id):
    props = {}
    path = os.path.join(TOPO_ROOT, str(node_id), "io_links", str(link_id), "properties")
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                props[parts[0]] = int(parts[1])
    return props


def discover_gpus():
    """Return sorted list of KFD node IDs that are GPUs."""
    gpus = []
    for entry in sorted(os.listdir(TOPO_ROOT), key=int):
        if read_node_property(int(entry), "simd_count") or 0 > 0:
            gpus.append(int(entry))
    return gpus


def generate_map(gpu_nodes):
    """Read xGMI IO links and extract recommended engine IDs."""
    node_to_gpu = {node: idx for idx, node in enumerate(gpu_nodes)}
    rows = []

    for src_node in gpu_nodes:
        src_gpu = node_to_gpu[src_node]
        links_dir = os.path.join(TOPO_ROOT, str(src_node), "io_links")
        for link_id in sorted(os.listdir(links_dir), key=int):
            props = read_link_properties(src_node, link_id)
            if props.get("type") != XGMI_LINK_TYPE:
                continue
            dst_node = props["node_to"]
            if dst_node not in node_to_gpu:
                continue
            mask = props.get("recommended_sdma_engine_id_mask", 0)
            if mask == 0 or (mask & (mask - 1)) != 0:
                print(f"  [WARN] GPU {src_gpu} -> GPU {node_to_gpu[dst_node]}: "
                      f"mask 0x{mask:x} is not a single engine, skipping",
                      file=sys.stderr)
                continue
            engine_id = int(math.log2(mask))
            rows.append((src_gpu, engine_id, node_to_gpu[dst_node]))

    return rows


def main():
    parser = argparse.ArgumentParser(
        description="Generate sDMA engine affinity map from KFD sysfs")
    parser.add_argument("--out", default="engineMap.csv",
                        help="Output CSV path (default: engineMap.csv)")
    parser.add_argument("--verify",
                        help="Verify output against an existing CSV")
    args = parser.parse_args()

    gpu_nodes = discover_gpus()
    if not gpu_nodes:
        sys.exit("[ERROR] No GPU nodes found in KFD topology")

    gfx_ver = read_node_property(gpu_nodes[0], "gfx_target_version")
    print(f"Found {len(gpu_nodes)} GPUs (gfx_target_version {gfx_ver})")
    for idx, node in enumerate(gpu_nodes):
        print(f"  GPU {idx} -> KFD node {node}")

    rows = generate_map(gpu_nodes)
    if not rows:
        sys.exit("[ERROR] No xGMI links with recommended_sdma_engine_id_mask found")

    with open(args.out, "w") as f:
        for src, engine, dst in rows:
            f.write(f"{src},{engine:02d},{dst}\n")
    print(f"Written {len(rows)} entries to {args.out}")

    if args.verify:
        actual = {}
        for line in open(args.verify):
            line = line.strip()
            if not line:
                continue
            s, e, d = map(int, line.split(","))
            actual[(s, d)] = e
        generated = {(s, d): e for s, e, d in rows}
        mismatches = 0
        for key in sorted(set(actual) | set(generated)):
            a = actual.get(key)
            g = generated.get(key)
            if a != g:
                print(f"  MISMATCH GPU{key[0]}->GPU{key[1]}: "
                      f"existing={a} generated={g}")
                mismatches += 1
        total = len(set(actual) | set(generated))
        print(f"Verify: {total - mismatches}/{total} match against {args.verify}")


if __name__ == "__main__":
    main()
