; Configuration file for Mini Kossel kit from Think3DPrint3D

; Communication and general
M111 S0                             ; Debug off
M550 PMy Mini Kossel		        ; Machine name (can be anything you like)
M551 Preprap                        ; Machine password (used for FTP)
M540 P0xBE:0xEF:0xDE:0xAD:0xFE:0xED ; MAC Address
;*** Adjust the IP address and gateway in the following 2 lines to suit your network
M552 P192.168.1.14                  ; IP address
M554 P192.168.1.1                   ; Gateway
M553 P255.255.255.0                 ; Netmask
M555 P2                             ; Set output to look like Marlin
G21                                 ; Work in millimetres
G90                                 ; Send absolute coordinates...
M83                                 ; ...but relative extruder moves

; Axis and motor configuration
M569 P0 S1							; Drive 0 goes forwards
M569 P1 S1							; Drive 1 goes forwards
M569 P2 S1							; Drive 2 goes forwards
M569 P3 S1							; Drive 3 goes forwards
M569 P4 S1							; Drive 4 goes forwards
M574 X2 Y2 Z2 P1					; set endstop configuration (all endstops at high end, active high)
;*** The homed height is deliberately set too high in the following - you will adjust it during calibration
M665 R105.6 L215.0 B85 H240			; set delta radius, diagonal rod length, printable radius and homed height
M666 X0 Y0 Z0						; put your endstop adjustments here
M92 X80 Y80 Z80						; Set axis steps/mm
M906 X800 Y800 Z800 E800			; Set motor currents (mA)
M201 X1000 Y1000 Z1000 E1000		; Accelerations (mm/s^2)
M203 X15000 Y15000 Z15000 E3600		; Maximum speeds (mm/min)
M566 X1200 Y1200 Z1200 E1200		; Maximum instant speed changes mm/minute

; Thermistors
;*** If you have a Duet board stickered "4.7K", change R1000 to R4700 to the following M305 commands
M305 P0 T100000 B3950 R1000 H30 L0	; Put your own H and/or L values here to set the bed thermistor ADC correction
M305 P1 T100000 B3974 R1000 H30 L0	; Put your own H and/or L values here to set the first nozzle thermistor ADC correction
M305 P2 T100000 B3974 R1000 H30 L0	; Put your own H and/or L values here to set the second nozzle thermistor ADC correction

; Tool definitions
M563 P1 D0 H1                       ; Define tool 1
G10 P1 S0 R0                        ; Set tool 1 operating and standby temperatures
M92 E663:663                       	; Set extruder steps per mm
;*** If you have a dual-nozzle build, un-comment the next 2 lines
;M563 P2 D1 H2                      ; Define tool 2
;G10 P2 S0 R0                       ; Set tool 2 operating and standby temperatures

// Z probe and compensation definition
;*** If you have an IR zprobe, change P0 to P1 in the following M558 command
M558 P0 X0 Y0 Z0					; Z probe is a switch and is not used for homing any axes
;G31 Z1.20 P500						; Set the IR zprobe height and threshold (put your own values here)
;*** Adjust the XY coordinates in the following M557 commands if necessary to suit your build and the position of the zprobe
M557 P0 X-50 Y-50                   ; Four... 
M557 P1 X-50 Y50                    ; ...probe points...
M557 P2 X50 Y50						; ...for bed...
M557 P3 X50 Y-50					; ...levelling
M557 P4 X0 Y0						; 5th probe point for levelling
;*** If you are using axis compensation, put the figures in the following command
M556 S78 X0 Y0 Z0                   ; Axis compensation here

