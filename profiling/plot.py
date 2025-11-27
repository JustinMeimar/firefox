from collections import defaultdict
from typing import List, Dict, Optional
from dataclasses import dataclass, field
import plotly.graph_objects as go

@dataclass
class PlotStyle:
    title_font_size: int = 16
    axis_font_size: int = 14
    legend_font_size: int = 12
    font_family: str = 'Arial, sans-serif'
    width: int = 1000
    height: int = 600
    scale: int = 2
    grid_color: str = 'rgba(0, 0, 0, 0.3)'
    grid_dash: str = 'dot'
    bg_color: str = 'white'
    line_width: int = 2
    marker_size: int = 4
    opacity: float = 0.7
    colors: List[str] = field(default_factory=lambda: [
        '#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd', 
        '#8c564b', '#e377c2', '#7f7f7f', '#bcbd22', '#17becf'
    ])
    op_colors: Dict[str, str] = field(default_factory=lambda: {
        'GetProp': '#FF6B6B',
        'GetElem': '#4ECDC4',
        'Add': '#45B7D1',  
        'JumpIfFalse': '#96CEB4',
        'Call': '#E91E63',
        'Lt': '#DDA15E',
        'ToNumeric': '#BC6C25',
        'StrictSetElem': '#9B59B6',
        'Inc': '#E74C3C',
        'GetGName': '#3498DB',
        'SetElem': '#2ECC71',
    })

class PlotBuilder:
    def __init__(self, style: Optional[PlotStyle] = None):
        self.style = style or PlotStyle()
        self.fig = go.Figure()
        self._color_idx = 0
    
    def next_color(self) -> str:
        color = self.style.colors[self._color_idx % len(self.style.colors)]
        self._color_idx += 1
        return color
    
    def add_line(self, x, y, name: str, color: Optional[str] = None):
        self.fig.add_trace(go.Scatter(
            x=x, y=y,
            mode='lines',
            name=name,
            line=dict(width=self.style.line_width, color=color or self.next_color()),
            marker=dict(size=self.style.marker_size),
            opacity=self.style.opacity
        ))
        return self
    
    def add_line_markers(self, x, y, name: str, color: Optional[str] = None):
        c = color or self.next_color()
        self.fig.add_trace(go.Scatter(
            x=x, y=y,
            mode='lines+markers',
            name=name,
            line=dict(width=self.style.line_width, color=c),
            marker=dict(size=self.style.marker_size, color=c)
        ))
        return self
    
    def add_vline(self, x: float, label: str):
        self.fig.add_vline(
            x=x, line_dash="dash", line_color="gray", opacity=0.5,
            annotation_text=label, annotation_position="top"
        )
        return self
    
    def set_layout(self, title: str, x_title: str, y_title: str, 
                   y_log: bool = False, x_range: Optional[List] = None):
        s = self.style
        self.fig.update_layout(
            title=dict(text=title, font=dict(size=s.title_font_size, family=s.font_family)),
            xaxis=dict(
                title=x_title,
                title_font=dict(size=s.axis_font_size, family=s.font_family),
                gridcolor=s.grid_color,
                griddash=s.grid_dash,
                range=x_range,
                showline=True, linewidth=1, linecolor='black'
            ),
            yaxis=dict(
                title=y_title,
                type='log' if y_log else 'linear',
                title_font=dict(size=s.axis_font_size, family=s.font_family),
                gridcolor=s.grid_color,
                griddash=s.grid_dash,
                showline=True, linewidth=1, linecolor='black'
            ),
            legend=dict(
                font=dict(size=s.legend_font_size),
                orientation='h',
                x=0.5, y=-0.15,
                xanchor='center', yanchor='top'
            ),
            width=s.width, height=s.height,
            plot_bgcolor=s.bg_color,
            hovermode='closest',
            margin=dict(l=60, r=20, t=60, b=80)
        )
        return self
    
    def save(self, filename: str):
        self.fig.write_image(filename, width=self.style.width, 
                            height=self.style.height, scale=self.style.scale)

def compute_percentiles(counts: List[int], percentiles: List[float]) -> Dict[float, int]:
    total = sum(counts)
    cumsum = 0
    positions = {}
    for i, count in enumerate(counts):
        cumsum += count
        pct = cumsum / total
        for p in percentiles:
            if p not in positions and pct >= p:
                positions[p] = i
    return positions

def plot_multi_distribution(datasets: Dict[str, List], combined: List, 
                           output: str):
    builder = PlotBuilder() 
    max_len = 0
    for name, stubs in datasets.items():
        counts = [s.call_count for s in stubs]
        max_len = max(max_len, len(counts))
        builder.add_line(list(range(len(counts))), counts, name)
    
    combined_counts = [s.call_count for s in combined]
    for pct, pos in compute_percentiles(combined_counts, [0.75, 0.9, 0.99]).items():
        builder.add_vline(pos, f"{int(pct*100)}%")
    
    builder.set_layout(
        title='IC Stub Call Count Distribution',
        x_title='Stub Rank',
        y_title='Call Count',
        y_log=True,
        x_range=[-10, max_len + 10]
    ).save(output)

def plot_op_distributions_multiline(stubs: List, output: str, title: str, top_k: int = 8):
    op_stubs = defaultdict(list)
    for stub in stubs:
        op_stubs[stub.op].append(stub)
    
    op_totals = {op: sum(s.call_count for s in lst) for op, lst in op_stubs.items()}
    sorted_ops = sorted(op_totals.items(), key=lambda x: x[1], reverse=True)[:top_k] 
    s = PlotStyle(width=1400, height=700, title_font_size=20,
                  axis_font_size=18, legend_font_size=16)
    builder = PlotBuilder(s)
    
    for op, _ in sorted_ops:
        op_list = sorted(op_stubs[op], key=lambda s: s.call_count, reverse=True)
        op_list = [s for s in op_list if s.call_count > 1]
        if not op_list:
            continue
        builder.add_line_markers(
            list(range(1, len(op_list) + 1)),
            [s.call_count for s in op_list],
            f'{op} ({len(op_list)})',
            color=s.op_colors.get(op, s.colors[hash(op) % len(s.colors)])
        )
    
    builder.set_layout(
        title=title,
        x_title='Stub Rank (Most to Least Frequent)',
        y_title='Call Count',
        y_log=True
    ).save(output)

def generate_op_distribution_table(stubs: List, output: str, title: str, top_k: int = 10):
    
    op_stubs = defaultdict(list)
    for stub in stubs:
        op_stubs[stub.op].append(stub)
    
    op_totals = {op: sum(s.call_count for s in lst) for op, lst in op_stubs.items()}
    sorted_ops = sorted(op_totals.items(), key=lambda x: x[1], reverse=True)[:top_k]
    
    ops = []
    total_calls = []
    stub_counts = []
    avg_calls = []
    max_calls = []
    
    for op, total in sorted_ops:
        op_list = sorted(op_stubs[op], key=lambda s: s.call_count, reverse=True)
        op_list = [s for s in op_list if s.call_count > 1]
        if not op_list:
            continue
        
        stub_count = len(op_list)
        avg = total / stub_count if stub_count > 0 else 0
        max_call = op_list[0].call_count if op_list else 0
        
        ops.append(f"<b>{op}</b>")
        total_calls.append(f"{total:,}")
        stub_counts.append(f"{stub_count:,}")
        avg_calls.append(f"{avg:,.0f}")
        max_calls.append(f"{max_call:,}")
    
    fig = go.Figure(data=[go.Table(
        header=dict(
            values=['<b>Op</b>', '<b>Total Calls</b>', '<b>Stub Count</b>', 
                    '<b>Avg Calls</b>', '<b>Max Calls</b>'],
            fill_color='#2C3E50',
            font=dict(color='white', size=14, family='Arial'),
            align='left',
            height=40
        ),
        cells=dict(
            values=[ops, total_calls, stub_counts, avg_calls, max_calls],
            fill_color=['#ECF0F1', '#FFFFFF'] * 3,
            font=dict(color='#2C3E50', size=12, family='Arial'),
            align=['left', 'right', 'right', 'right', 'right'],
            height=35
        )
    )])
    
    fig.update_layout(
        title=dict(
            text=title,
            font=dict(size=18, family='Arial', color='#2C3E50'),
            x=0.5,
            xanchor='center'
        ),
        width=1200,
        height=600,
        margin=dict(l=20, r=20, t=80, b=20)
    )
    
    fig.write_image(output, width=1200, height=600, scale=2)

