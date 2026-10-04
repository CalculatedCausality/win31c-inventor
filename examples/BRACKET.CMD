# L-shaped angle bracket with a box gusset in the inside corner.
# Run from CAD.EXE: Tools > Run Macro. All coordinates stay within -400..400.
CADSCRIPT 1
UNITS MM
SET LEG 120
SET T 20
SET WIDTH 60
# L profile in XY (two legs of thickness T), extruded WIDTH along +Z.
EXTRUDE WIDTH 0,0 LEG,0 LEG,T T,T T,LEG 0,LEG
# Gusset block filling the inside corner, centred across the width.
SET G 40
BOX T T (WIDTH-T)/2 G G T
