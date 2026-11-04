#!/usr/bin/env python3
# CPU-measured per-root exec time vs degree, with a zoom inset over the light-degree region.
# Source: result/degcpu_<pattern>_<GRAPH>.csv (node,degree,ns).
# Output: figure/deg_<pattern>_<SUFFIX>_cpu.{png,pdf,eps}
import csv, sys, numpy as np, math
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42
CHUNK=50
GRAPH=sys.argv[1] if len(sys.argv)>1 else 'cit-Patents'
SUFFIX=sys.argv[2] if len(sys.argv)>2 else 'PT'
def nice(vmax):
    step=10**math.floor(math.log10(vmax))
    for s in (step,step*2,step*5,step*10):
        if vmax/s<=3: step=s; break
    top=math.ceil(vmax/step)*step
    ticks=[round(step*k,10) for k in range(int(top/step)+1)]
    return top, ticks
PATS={'HOUSE5':('house5','#CC79A7'),   # P1
      'P_P6':('p_p6','#D55E00')}      # P4
for pk,(pfile,c) in PATS.items():
    f=f'result/degcpu_{pfile}_{GRAPH}.csv'
    if not __import__('os').path.exists(f):
        sys.exit(f"{SUFFIX}/{pk}: no {f} (run the degcpu experiment first)")
    deg=[];t=[]
    with open(f) as fh:
        r=csv.reader(fh); next(r)
        for row in r:
            try: dd=int(row[1]); ns=int(row[2])
            except: continue
            deg.append(dd); t.append(ns/1e6)          # ns -> ms (CPU wall)
    deg=np.array(deg); t=np.array(t)
    order=np.argsort(deg,kind='stable'); ts=t[order]; ds=deg[order]; N=len(ts)
    cm=np.array([ts[i:i+CHUNK].mean() for i in range(0,N,CHUNK)]); nc=len(cm); dmax=int(ds[-1])
    pos=[np.searchsorted(ds,m)/CHUNK for m in (1,2,4,8,16)]+[nc-1]
    b=int(np.searchsorted(ds,16)); nlight=b//CHUNK
    FS=26
    iymax=cm[:nlight].max(); iyl,iyt=nice(iymax)
    fig,ax=plt.subplots(figsize=(7.4,5.4))
    ax.plot(np.arange(nc),cm,color=c,lw=1.1)
    ax.set_xticks(pos); ax.set_xticklabels(['1','2','4','8','16',str(dmax)],fontsize=FS)
    ax.tick_params(axis='y',labelsize=FS)
    ax.set_xlabel("root vertex's degree",fontsize=29)
    ax.set_ylabel('execution time (ms)',fontsize=33)
    ax.ticklabel_format(style='plain',axis='y')
    axin=ax.inset_axes([0.20,0.44,0.48,0.50], xlim=(0,nlight-1), ylim=(0,iyl))
    axin.plot(np.arange(nc),cm,color=c,lw=1.0)
    posb=[np.searchsorted(ds,m)/CHUNK for m in (1,2,4,8)]+[nlight-1]
    axin.set_xticks(posb); axin.set_xticklabels(['1','2','4','8','16'],fontsize=FS)
    axin.set_yticks(iyt); axin.tick_params(axis='y',labelsize=FS)
    ax.indicate_inset_zoom(axin, edgecolor='gray')
    out=f'figure/deg_{pk}_{SUFFIX}_cpu'
    fig.subplots_adjust(left=0.15, right=0.965, bottom=0.185, top=0.965)
    plt.savefig(out+'.png',dpi=200); plt.savefig(out+'.pdf'); plt.savefig(out+'.eps',format='eps'); plt.close()
    print(f"{SUFFIX}/{pk}: CPU per-root, inset deg 1..15, ymax={iymax:.4f}ms -> {out}")
