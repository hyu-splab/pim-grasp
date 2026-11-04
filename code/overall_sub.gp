# Overall performance as four separate subfigures (one .eps/.pdf/.png per dataset).
# Vertical clustered bars, P1..P5, three engines. Legend drawn only on the PT subfigure.
#   gnuplot overall_sub.gp  ->  ../../figure/overall_{PT,CJ,LJ,PK}.{eps,pdf,png}
set style data histograms
set style histogram clustered gap 1.2
set style fill solid 0.92 border lc rgb '#333333'
set boxwidth 1.0 relative
# 5 clusters at x=0..4; tight x-range trims side whitespace
set xrange [-0.55:4.55]

# B&W-safe palette by luminance: light (baseline) -> mid -> dark (hero)
C_PIMPAM = '#dce6f0'   # PimPam       -- light blue
C_NAIVE  = '#6b93c0'   # OFIS-PimPam  -- medium blue
C_OFIS   = '#1f3b5c'   # PIM-grasp    -- dark navy

set grid ytics lc rgb '#bbbbbb' lt 1 lw 0.8
set border lw 1.0
set ytics nomirror font ",12"
set xtics nomirror scale 0 font ",12" noenhanced

if (!exists("GRAPHS")) GRAPHS = "PT CJ LJ PK"
# per-dataset y max, keyed by NAME so any subset/order of GRAPHS renders correctly
ymax(g) = (g eq "PT") ? 48 : (g eq "CJ") ? 150 : (g eq "LJ") ? 275 : (g eq "PK") ? 50 : 100

# All four subfigures share the same total size; only the left margin differs (ylabel vs none).

do for [gi=1:words(GRAPHS)] {
  GRAPH = word(GRAPHS, gi)
  DAT = 'overall_p14_'.GRAPH.'.dat'
  # keep the per-dataset axis unless a measured bar would be clipped by it
  set yrange [*:*]   # stats honours the range left by the previous dataset
  stats DAT using 2 nooutput; DMAX = STATS_max
  stats DAT using 3 nooutput; if (STATS_max > DMAX) { DMAX = STATS_max }
  stats DAT using 4 nooutput; if (STATS_max > DMAX) { DMAX = STATS_max }
  GROWN = (DMAX > ymax(GRAPH))
  set yrange [0:(GROWN ? DMAX * 1.05 : ymax(GRAPH))]
  unset title
  set label 99 GRAPH at graph 0.955,0.90 right font ",11" front noenhanced
  if (!GROWN && GRAPH eq "PT") { set ytics 0,10,40 nomirror font ",12" } \
  else { if (!GROWN && GRAPH eq "CJ") { set ytics 0,50,150 nomirror font ",12" } \
  else { set ytics autofreq nomirror font ",12" } }
  # y-axis label on the left column only (PT, PK)
  IS_LEFT = (GRAPH eq "PT" || GRAPH eq "PK")
  if (GRAPH eq "PT") { set ylabel "E2E time (sec)" font ",12" offset 1.0,0 }
  if (GRAPH eq "PK") { set ylabel "E2E time (sec)" font ",12" offset 1.0,0 }
  if (!IS_LEFT) { unset ylabel }
  if (gi==1) { set key at graph 0.05,0.955 left top reverse Left samplen 1.3 spacing 1.2 font ",11" } \
  else { unset key }
  LM = IS_LEFT ? 0.185 : 0.120
  do for [term=1:3] {
    if (term==1) { set terminal pdfcairo size 2.05in,1.62in font "Helvetica,13" linewidth 1.0; set output '../../figure/overall_'.GRAPH.'.pdf' } \
    else { if (term==2) { set terminal pngcairo size 410,324 font "Helvetica,13" linewidth 1.0; set output '../../figure/overall_'.GRAPH.'.png' } \
    else { set terminal epscairo size 2.05in,1.62in font "Helvetica,13" linewidth 1.0; set output '../../figure/overall_'.GRAPH.'.eps' } }
    set lmargin at screen LM
    set rmargin at screen 0.988
    set bmargin at screen 0.183
    set tmargin at screen 0.950
    plot DAT using 2:xtic(1) title 'PimPam'      lc rgb C_PIMPAM, \
         ''  using 3         title 'OFIS-PimPam' lc rgb C_NAIVE,  \
         ''  using 4         title 'PIM-grasp'   lc rgb C_OFIS
    unset output
  }
}
