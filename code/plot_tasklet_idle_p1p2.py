#!/usr/bin/env python3
# Intra-DPU tasklet idle, on-demand vs static, P1 (HOUSE5) and P2 (HOUSE5_X).
# Each bar = on-demand idle as a fraction of static (dashed 1.0 baseline).
import os, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
matplotlib.rcParams["font.family"]="sans-serif"
matplotlib.rcParams["font.sans-serif"]=["Liberation Sans","Arial","Helvetica","DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"]=42
matplotlib.rcParams["ps.fonttype"]=42
HERE=os.path.dirname(os.path.abspath(__file__)); ROOT=os.path.join(HERE,"..")
LB=os.path.join(ROOT,"result","lb"); OUT=os.path.join(ROOT,"sections","figures")
DATASETS=[g for g in ["PT","CJ","PK","LJ"] if g in os.environ.get("GRAPHS","PT CJ PK LJ").split()]
CLK=350e6

def ratio(pat,g):   # on/off idle ratio
    def s(t):
        f=os.path.join(LB,f"idle_{pat}_{g}_{t}.dpuk")
        return sum(int(l.split()[2]) for l in open(f) if l.startswith("DPUK"))/CLK
    o=s("off"); return s("on")/o if o else 0

P1=[ratio("HOUSE5",g)   for g in DATASETS]
P2=[ratio("HOUSE5_X",g) for g in DATASETS]

C_P1, C_P2 = "#a9c0d6", "#39627d"
FX, FY, FL = 25, 26, 24
x=np.arange(len(DATASETS)); w=0.38
fig,ax=plt.subplots(figsize=(6.4,4.4),constrained_layout=True)
ax.bar(x-w/2, P1, w, color=C_P1, edgecolor="#222", linewidth=0.9, label="P1")
ax.bar(x+w/2, P2, w, color=C_P2, edgecolor="#222", linewidth=0.9, label="P2")
# static baseline as a single dashed line + label
ax.axhline(1.0, ls="--", color="black", lw=1.4)
ax.text(len(DATASETS)-0.5, 1.02, "static assignment", ha="right", va="bottom", fontsize=25, color="black")
ax.set_xticks(x); ax.set_xticklabels(DATASETS, fontsize=FX)
ax.set_ylabel("Norm. tasklet idle time", fontsize=FL)
ax.tick_params(axis="y", labelsize=FY)
ax.set_ylim(0, max(1.18, max(P1 + P2) * 1.05)); ax.set_yticks([0.0,0.2,0.4,0.6,0.8,1.0])
ax.legend(fontsize=22, frameon=False, loc="upper left", ncol=2, columnspacing=0.8,
          handlelength=0.9, handletextpad=0.3, borderaxespad=0.1)
for s in ("top","right"): ax.spines[s].set_visible(False)
out=os.path.join(OUT,"tasklet_idle")
for e in ("png","pdf","eps"): fig.savefig(f"{out}.{e}", dpi=300, bbox_inches="tight")
print("wrote sections/figures/tasklet_idle.{png,pdf,eps}")
for i,g in enumerate(DATASETS): print(f"  {g}: P1={P1[i]:.3f}  P2={P2[i]:.3f}")
