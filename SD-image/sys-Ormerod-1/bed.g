; bed.g
; called to perform automatic bed compensation
M561
G1 Z5 F360
G1 X40 Y40 F12000
G30 P0 Z-10000
G1 Z5 F360
G1 X40 Y180 F12000
G30 P1 Z-10000
G1 Z5 F360
G1 X200 Y180 F12000
G30 P2 Z-10000
G1 Z5 F360
G1 X200 Y40 F12000
G30 P3 Z-10000 S ; if using 5 probe points, remove the last 'S' parameter
;G1 Z5 F360      ; and uncomment the following lines
;G1 X120 Y97.5 F12000
;G30 P4 Z-10000 S
G1 Z5 F360
