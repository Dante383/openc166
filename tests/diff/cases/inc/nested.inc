; Included from regs.inc. Keil looks up nested names relative to the main
; source, not to the including file.
NESTED  EQU     INIT+1
$INCLUDE (inc\sub\level3.inc)
