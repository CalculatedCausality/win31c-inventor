# Simple table: one top slab and four square legs, sized by variables.
# Run from CAD.EXE: Tools > Run Macro. All coordinates stay within -400..400.
CADSCRIPT 1
UNITS MM
SET W 300
SET D 200
SET H 180
SET TOP 20
SET LEG 20
SET X0 -W/2
SET Y0 -D/2
SET X1 W/2-LEG
SET Y1 D/2-LEG
SET LEGH H-TOP
# Top slab: W along X, D along Y, TOP thick, upper face at Z=H.
BOX X0 Y0 LEGH W D TOP
# Legs from the floor (Z=0) up to the underside of the top.
BOX X0 Y0 0 LEG LEG LEGH
BOX X1 Y0 0 LEG LEG LEGH
BOX X0 Y1 0 LEG LEG LEGH
BOX X1 Y1 0 LEG LEG LEGH
