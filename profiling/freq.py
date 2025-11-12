#-------=====================================================----------#
#                  Justin's Stub Frequency Script.
#   
#       Stubs are generated from running the browser in debug with
#       the following environment variables.
#
#           MOZ_DISABLE_CONTENT_SANDBOX=1
#           IC_STAT_LOG_DIR=<path-to-stubs>
#           
#-------=====================================================----------#
from pathlib import Path
import matplotlib.pyplot as plt
import json

def load_stub_logs(stub_dir: Path): 
    content_stubs = []
    parent_stubs = []
    for path in stub_dir.iterdir():
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

def compute_distribution(stubs):
    stubs = fold_duplicate_stubs(stubs)
    stubs.sort(key=lambda stub: stub["call-count"], reverse=True) 
    print(stubs[0:5]) 
    return stubs

def make_content_parent_graph(content_stubs, parent_stubs):
    """
    Plot call count distributions for content vs parent processes.
    """
    content_sorted = compute_distribution(content_stubs)
    parent_sorted = compute_distribution(parent_stubs)
    
    plt.figure(figsize=(10, 6))
    plt.plot([s["call-count"] for s in content_sorted], label="Content", alpha=0.7)
    plt.plot([s["call-count"] for s in parent_sorted], label="Parent", alpha=0.7)
    
    plt.xlabel("Stub Rank")
    plt.ylabel("Call Count")
    plt.title("IC Stub Call Count Distribution")
    plt.legend()
    plt.yscale("log")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    content, parent = load_stub_logs(Path("./stubs"))
    print("computing parent process stub distribution...") 
    parent_stubs = compute_distribution(parent)

    print("computing content process stub distribution...") 
    content_stubs = compute_distribution(content)
    make_content_parent_graph(content_stubs, parent_stubs)

