from typing import List, Dict
from pathlib import Path
from argparse import ArgumentParser
from dataclasses import dataclass
import json
from plot import *

@dataclass
class Stub:
    call_count: int
    hash: int 
    ir: List[str]
    op: str = None
    call_ratio: float = None

def load_stubs_from_dir(dir: Path) -> List[Stub]:
    stubs = []
    stub_map = {}
    for path in dir.iterdir():
        if not path.is_file():
            continue
        with path.open('r') as f:
            data = json.load(f)
            for entry in data["entries"]: 
                for stub_data in entry["stubs"]:
                    if stub_data["call-count"] <= 0:
                        continue
                    hash_val = stub_data["hash"]
                    if hash_val in stub_map:
                        stub_map[hash_val].call_count += stub_data["call-count"]
                    else:
                        stub_map[hash_val] = Stub(
                            call_count=stub_data["call-count"],
                            hash=hash_val,
                            ir=stub_data["ir"],
                            op=entry["op"]
                        )
    return list(stub_map.values())

def fold_duplicate_stubs(stubs: List[Stub]) -> List[Stub]:
    dedup = {}
    for stub in stubs:
        if stub.hash in dedup:
            dedup[stub.hash].call_count += stub.call_count  
        else:
            dedup[stub.hash] = stub
    return list(dedup.values())

def update_normalized_count(stubs: List[Stub]) -> List[Stub]:
    total = sum(s.call_count for s in stubs)
    for stub in stubs:
        stub.call_ratio = round(stub.call_count / total, 3) if total else 0
    return stubs

def compute_distribution(stubs: List[Stub]) -> List[Stub]:
    # stubs = fold_duplicate_stubs(stubs)
    stubs.sort(key=lambda s: s.call_count, reverse=True) 
    return stubs

if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("--speedometer", required=True, help="path to speedometer stub directory")
    parser.add_argument("--jetstream", required=True, help="path to jetstream stub directory")
    parser.add_argument("--parent", required=True, help="path to parent stub directory")
    parser.add_argument("--combined-name", default="combined")
    args = parser.parse_args()
    
    datasets: Dict[str, List[Stub]] = {}
    for name, path in [
        ("Speedometer3", args.speedometer),
        ("JetStream2", args.jetstream),
        ("Parent Process", args.parent)]:
        d = Path(path)
        if not d.is_dir():
            print(f"Invalid directory: {path}")
            exit(1)
        datasets[name] = compute_distribution(load_stubs_from_dir(d))
    combined = compute_distribution([s for stubs in datasets.values() for s in stubs])
   
    print("Plotting Parent Content Graph")
    plot_multi_distribution(datasets, combined, "parent_content_plot.png") 
    
    print("Plotting Speed Op Dist")
    plot_op_distributions_multiline(
            stubs=datasets["Speedometer3"],
            output="speedometer_op_dist.png",
            title="Stub Call Distribution - Speedometer 3 (Top 10 Ops)",
            top_k=10)

    print("Plotting JetStream2 Op Dist")
    plot_op_distributions_multiline(
            stubs=datasets["JetStream2"],
            output="jetstream_op_dist.png",
            title="Stub Call Distribution - JetStream 2 (Top 10 Ops)",
            top_k=10)
    
    print("Plotting Parent Process Op Dist")
    plot_op_distributions_multiline(
            stubs=datasets["Parent Process"],
            output="parent_op_dist.png",
            title="Stub Call Distribution - Parent Process (Top 10 Ops)",
            top_k=10)

    print("Generating Speedometer3 table")
    generate_op_distribution_table(
        stubs=datasets["Speedometer3"],
        output="speedometer_op_table.png",
        title="Speedometer 3 - Top 10 Op Distribution Summary",
        top_k=10)

    print("Generating JetStream2 table")
    generate_op_distribution_table(
        stubs=datasets["JetStream2"],
        output="jetstream_op_table.png",
        title="JetStream 2 - Top 10 Op Distribution Summary",
        top_k=10)

    print("Generating Parent Process table")
    generate_op_distribution_table(
        stubs=datasets["Parent Process"],
        output="parent_op_table.png",
        title="Parent Process - Top 10 Op Distribution Summary",
        top_k=10)

