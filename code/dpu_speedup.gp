# DPU-count speedup: speedup vs No. of DPUs, one line per pattern (P1..P5), one dataset per run.
# Data: dpu_speedup_<G>.dat. Usage: gnuplot -e "GRAPH='PT'" dpu_speedup.gp
reset
if (!exists("GRAPH")) GRAPH = "PT"
if (!exists("YMAX"))   YMAX   = 4
if (!exists("YTIC"))   YTIC   = 1
if (!exists("YSTART")) YSTART = 1
DAT = 'dpu_speedup_'.GRAPH.'.dat'
# keep the axis unless a measured speedup would be clipped by it
DMAX = 0
do for [c=2:6] { stats DAT using c nooutput; if (STATS_max > DMAX) { DMAX = STATS_max } }
if (DMAX > YMAX) { YMAX = ceil(DMAX) }

# graph name inside the plot (top-centre)
unset title
set label 1 GRAPH at graph 0.5,0.86 center font ",20"
set size 1,0.5
# fixed margins so the plot box is identical for PT and LJ whether or not the x-label is present
set lmargin 5.5
set rmargin 1.5
set tmargin 1.0
set bmargin 3.0
set xrange [0:1100]
set yrange [0:YMAX]
set ylabel "Speedup" offset 1,0
# PT stacks above LJ, which carries the shared x-label
if (GRAPH eq "PT") { unset xlabel } else { set xlabel "No. of DPUs" offset 0,0.5 }
set ytics YSTART,YTIC,YMAX
set xtics (64,128,256,512,1024) nomirror
set grid y
# legend on PT only
if (GRAPH eq "PT") { set key bottom horizontal } else { unset key }

set terminal postscript eps enhanced color 22
set output '../../figure/dpu_speedup_'.GRAPH.'.eps'
plot DAT u 1:2 w lp lw 3 ps 1.5 t "P1", \
     ''  u 1:3 w lp lw 3 ps 1.5 t "P2", \
     ''  u 1:4 w lp lw 3 ps 1.5 t "P3", \
     ''  u 1:5 w lp lw 3 ps 1.5 t "P4", \
     ''  u 1:6 w lp lw 3 ps 1.5 t "P5"
unset output

# PNG preview
set size 1,1
set terminal pngcairo enhanced size 900,300 font "Helvetica,16"
set output '../../figure/dpu_speedup_'.GRAPH.'.png'
replot
unset output

# PDF
set terminal pdfcairo enhanced size 5.0in,1.7in font "Helvetica,12"
set output '../../figure/dpu_speedup_'.GRAPH.'.pdf'
replot
unset output
