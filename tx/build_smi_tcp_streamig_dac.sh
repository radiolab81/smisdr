#gcc -O2 -I/usr/src/linux-headers-6.12.47+rpt-common-rpi/include/ \
# smi_tcp_streaming_dac.c -o smi_tcp_streaming_dac -lpthread

gcc -O3 -Wall -D_GNU_SOURCE smi_tcp_streaming_dac.c -o smi_tcp_streaming_dac -lpthread

