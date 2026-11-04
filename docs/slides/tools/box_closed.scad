// openscad -D 'part="none"' --preview --viewall --autocenter --projection=p --camera=0,0,0,55,0,25,0 --imgsize=1800,1100 --colorscheme=Tomorrow -o box.png box_closed.scad
// closed-lid render for the slides; geometry comes unchanged from airguard_box.scad
include <../../../hardware/enclosure_v2/airguard_box.scad>
color([0.93, 0.91, 0.86]) body();
color([0.88, 0.86, 0.81]) lid();
for (s = sw_xy) color([0.6, 0.6, 0.6]) translate([s[0], s[1], peg_bot]) peg();
