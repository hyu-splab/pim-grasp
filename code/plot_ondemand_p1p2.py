#!/usr/bin/env python3
# Intra-DPU on-demand effect on per-DPU kernel time, P1 (HOUSE5) and P2 (HOUSE5_X).
# Each bar = on-demand mean per-DPU kernel time as a fraction of static (dashed 1.0 baseline).
import os, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
matplotlib.rcParams["font.family"]="sans-serif"
matplotlib.rcParams["font.sans-serif"]=["Liberation Sans","Arial","Helvetica","DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"]=42
matplotlib.rcParams["ps.fonttype"]=42
HERE=os.path.dirname(os.path.abspath(__file__)); ROOT=os.path.join(HERE,"..")
OUT=os.path.join(ROOT,"sections","figures")
DATASETS=[g for g in ["PT","CJ","PK","LJ"] if g in os.environ.get("GRAPHS","PT CJ PK LJ").split()]

def ratio(pdir,g):   # on/off mean per-DPU kernel time over active DPUs
    def m(t):
        d=np.genfromtxt(os.path.join(ROOT,"paper_figure_data",pdir,f"perdpu_{g}_{t}.csv"),
                        delimiter=",",names=True)
        k=np.atleast_1d(d["kernel_ms"]); k=k[k>0]; return k.mean()
    o=m("off"); return m("on")/o if o else 0

P1=[ratio("ondemand_p1",g) for g in DATASETS]
P2=[ratio("ondemand_p2",g) for g in DATASETS]

C_P1, C_P2 = "#a9c0d6", "#39627d"
FX, FY, FL = 25, 26, 24
x=np.arange(len(DATASETS)); w=0.38
fig,ax=plt.subplots(figsize=(6.4,4.4),constrained_layout=True)
ax.bar(x-w/2, P1, w, color=C_P1, edgecolor="#222", linewidth=0.9, label="P1")
ax.bar(x+w/2, P2, w, color=C_P2, edgecolor="#222", linewidth=0.9, label="P2")
ax.axhline(1.0, ls="--", color="black", lw=1.4)
ax.set_xticks(x); ax.set_xticklabels(DATASETS, fontsize=FX)
ax.set_ylabel("Norm. DPU exec. time", fontsize=FL)
ax.tick_params(axis="y", labelsize=FY)
ax.set_ylim(0, max(1.18, max(P1 + P2) * 1.05)); ax.set_yticks([0.0,0.2,0.4,0.6,0.8,1.0])
for s in ("top","right"): ax.spines[s].set_visible(False)
out=os.path.join(OUT,"ondemand_mean")
for e in ("png","pdf","eps"): fig.savefig(f"{out}.{e}", dpi=300, bbox_inches="tight")
print("wrote sections/figures/ondemand_mean.{png,pdf,eps}")
for i,g in enumerate(DATASETS): print(f"  {g}: P1={P1[i]:.3f}  P2={P2[i]:.3f}")
