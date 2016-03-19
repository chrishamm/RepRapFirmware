; pause.g
; Put G/M Codes in here to run when a print is paused using M25 or M226
M83
G1 E-10 F3600
G91
G1 Z+5 F360
G90
G1 X0 Y0 F9000
