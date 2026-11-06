# board/renesas/common/Makefile already builds rza1-common.o and
# rza1-lowlevel_init.o under CONFIG_RZA1, and U-Boot descends into that directory
# on its own. Referencing ../common/rza1-common.o here as well builds the same
# object twice, concurrently, and the two writers corrupt it — which surfaces as
# scripts/check-local-export failing on a truncated object file.
# grpeach, the other RZ/A1 board, likewise lists only its own object.
obj-y	:= deluge.o
obj-y	+= save_boot_params.o
