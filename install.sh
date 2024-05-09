make clean
make
make install
depmod -a
modprobe gslx680_ts_acpi
