
# CMSIS-DAP OVER ETHERNET

- implemented based on esp8266 sdk 3
- please specify your ssid and password in ```main.c```
- work with patched openocd at https://github.com/ReinForce-II/openocd, initialize it with command "interface cmsis-dap-eth; cmsis_dap_addr x.x.x.x; cmsis_dap_port yyyy"
- almost no failover done, such as connection loss, etc 
