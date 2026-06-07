IVERILOG = iverilog
VVP      = vvp

RTL = ../rtl

.PHONY: all sim_autoconfig sim_registers sim_boardram sim_bridge_e2e \
        sim_regfile sim_ddram clean

all: sim_autoconfig sim_boardram

sim_autoconfig: tb_autoconfig
	$(VVP) tb_autoconfig

tb_autoconfig: tb_autoconfig.v $(RTL)/a2065_autoconfig.v
	$(IVERILOG) -o $@ $^

sim_registers: tb_registers
	$(VVP) tb_registers

tb_registers: tb_registers.v $(RTL)/a2065_registers.v
	$(IVERILOG) -o $@ $^

sim_boardram: tb_boardram
	$(VVP) tb_boardram

tb_boardram: tb_boardram.v $(RTL)/a2065_boardram.v
	$(IVERILOG) -o $@ $^

sim_bridge_e2e: tb_bridge_e2e
	$(VVP) tb_bridge_e2e

tb_bridge_e2e: tb_bridge_e2e.v $(RTL)/a2065_registers.v
	$(IVERILOG) -o $@ $^

sim_regfile: tb_regfile
	$(VVP) tb_regfile

tb_regfile: tb_regfile.v $(RTL)/a2065_regfile.v
	$(IVERILOG) -o $@ $^

sim_ddram: tb_ddram
	$(VVP) tb_ddram

tb_ddram: tb_ddram.v $(RTL)/a2065_ddr_window.v
	$(IVERILOG) -o $@ $^

clean:
	rm -f tb_autoconfig tb_registers tb_boardram tb_bridge_e2e \
	      tb_regfile tb_ddram *.vcd
