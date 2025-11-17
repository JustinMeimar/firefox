
import matplotlib.pyplot as plt

def plot_content_parent_distribution(content_stubs, parent_stubs, png_name):
    """
    Plot call count distributions for content vs parent processes.
    """
    from freq import compute_distribution
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
    plt.savefig(png_name)

def plot_stub_proportions_pie(stubs, png_name):
    """
    Plot call-ratio distribution as a pie chart.
    """
    total_calls = sum(s["call-count"] for s in stubs)
    top_n = 20
    top_stubs = stubs[:top_n]
    other_calls = sum(s["call-count"] for s in stubs[top_n:])
    labels = []
    sizes = []
    for s in top_stubs:
        ratio = s["call-count"] / total_calls
        labels.append(f"{s['hash']}")
        sizes.append(ratio)
    if other_calls > 0:
        labels.append("Other")
        sizes.append(other_calls / total_calls)
    
    plt.figure(figsize=(10, 6))
    plt.pie(sizes, labels=labels, autopct='%1.1f%%')
    plt.title("IC Stub Call Ratio Distribution")
    plt.tight_layout()
    plt.savefig(png_name)

def plot_stub_distribution(stubs, process_name, output_name, top_n=20):
    """
    """
    fig, ax = plt.subplots(figsize=(8, 10))
    top_stubs = stubs[:top_n]
    def create_label(stub):
        op = stub.get('maybe-op', 'Unknown')
        return f"({op})"
    
    labels = [create_label(s) for s in top_stubs]
    ratios = [s['call-ratio'] for s in top_stubs]
    color = '#2E86AB' if 'parent' in process_name.lower() else '#A23B72'
    
    bars = ax.barh(range(len(top_stubs)), ratios, color=color, alpha=0.8)
    ax.set_yticks(range(len(top_stubs)))
    ax.set_yticklabels(labels, fontsize=9)
    ax.set_xlabel('Call Frequency Ratio', fontsize=11, fontweight='bold')
    ax.invert_yaxis()
    ax.grid(axis='x', alpha=0.3, linestyle='--')
    for i, (bar, ratio) in enumerate(zip(bars, ratios)):
        ax.text(ratio, i, f' {ratio:.1%}', va='center', fontsize=8)
    
    coverage = sum(ratios)
    ax.set_title(
        f'{process_name} IC Stub Distribution (Top {top_n})\n'
        f'Coverage: {coverage:.1%}',
        fontsize=12, fontweight='bold', pad=15
    )
    plt.tight_layout()
    plt.savefig(f'{output_name}.png', dpi=300, bbox_inches='tight')
    plt.close()

