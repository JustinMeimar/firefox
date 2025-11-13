#-------=====================================================----------#
#                   
#                       Stub Frequency Script.
#   
#       Stubs are generated from running the browser in debug with
#                  the following environment variables.
#
#            - MOZ_DISABLE_CONTENT_SANDBOX=1
#            - IC_STAT_LOG_DIR=<path-to-stubs>
#           
#-------=====================================================----------#

from typing import List
from pathlib import Path
from argparse import ArgumentParser
import matplotlib.pyplot as plt
import json
from plot import *

def load_stub_logs(stub_dir: List[Path]): 
    content_stubs = []
    parent_stubs = []
    for dir in stub_dirs: 
        for path in dir.iterdir():
            with path.open('r') as stub_log_f:
                data = json.load(stub_log_f)
                for entry in data["entries"]: 
                    for stub in entry["stubs"]:
                        stub["maybe-op"] = entry["op"]
                        if "content" in path.name:
                            content_stubs.append(stub)
                        elif "parent" in path.name:
                            parent_stubs.append(stub) 
    return content_stubs, parent_stubs 

def fold_duplicate_stubs(stubs):
    """
    An identical stub may appear in different JitScripts. Here we sum their
    call-counts to give the total call-count for each.
    """
    dedup_stubs = {}
    for stub in stubs:
        stub_hash = stub["hash"]
        if stub_hash in dedup_stubs:
            dedup_stubs[stub_hash]["call-count"] += stub["call-count"]  
        else:
            dedup_stubs[stub_hash] = stub
    return list(dedup_stubs.values())


def update_normalized_count(stubs):
    total_count = sum([stub["call-count"] for stub in stubs])
    for stub in stubs:
        stub["call-ratio"] = round(stub["call-count"] / total_count, 3)
    return stubs

def compute_distribution(stubs):
    stubs = fold_duplicate_stubs(stubs)
    stubs = update_normalized_count(stubs)
    stubs.sort(key=lambda stub: stub["call-count"], reverse=True) 
    print(stubs[0:5]) 
    return stubs

if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("stub_dirs", nargs="+")
    args = parser.parse_args()
    if not args.stub_dirs:
        print("must supply stub_dirs to analyze")
        exit(1)
    
    def verify_dir_or_exit(dir) -> Path:
        d = Path(dir)
        if not d.is_dir():
            print(f"Supplied directory: {dir} is invalid.")
            exit(1)
        return d
    
    stub_dirs = list(map(verify_dir_or_exit, args.stub_dirs))
    content_combine, parent_combine = load_stub_logs(stub_dirs)
     
    print("computing parent process stub distribution...") 
    parent_stubs = compute_distribution(parent_combine)
    
    print("computing content process stub distribution...") 
    content_stubs = compute_distribution(content_combine)
    
    hash_suffix = str(abs(hash(''.join(args.stub_dirs))))[0:8]

    # Uncomment to plot
    
    # plot_stub_proportions_pie(parent_stubs, f"stub_pie_parent_{hash_suffix}", 20)
    # plot_stub_proportions_pie(content_stubs, f"stub_pie_content_{hash_suffix}", 20)
    # plot_stub_distribution(parent_stubs, "parent", f"stub_dist_parent_{hash_suffix}")
    # plot_stub_distribution(content_stubs, "content", f"stub_dist_content_{hash_suffix}")

