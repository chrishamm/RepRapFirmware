; homex.g
; called to home the X axis
G91
G1 Z3 F360
G90
;M558 P1 ; uncomment this line if you upgrade to a 4-wire probe
G1 X-240 F2400 S1
G91
G1 X6
G90
G1 X-30 F300 S1
;M558 P2 ; uncomment this line if you upgrade to a 4-wire probe
G91
G1 Z-3 F360
G90
