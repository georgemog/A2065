# ============================================================
# a2065.sdc — Timing constraints for A2065 Ethernet emulation
# ============================================================
#
# The A2065 module runs entirely in the clk_sys (~28.375 MHz)
# domain.  ARM bridge signals cross from h2f_user0_clk
# (100 MHz) via the HPS2FPGA lightweight bridge.
#
# Main PLL clocks are already derived by Minimig.sdc via
# derive_pll_clocks.  This file adds cross-domain constraints
# specific to the A2065 module.
#
# Hierarchy (when integrated into Minimig):
#   emu|minimig|a2065_top|u_boardram|*
#   emu|minimig|a2065_top|u_autoconfig|*
#   emu|minimig|a2065_top|u_registers|*
# ============================================================

# ----------------------------------------------------------
# 1. False paths: ARM boardram bridge inputs
#    HPS 100 MHz domain -> FPGA clk_sys domain.
#    The HPS2FPGA lightweight bridge provides adequate
#    synchronization for single-cycle writes.
# ----------------------------------------------------------
set_false_path -from {emu|minimig|*a2065*|arm_boardram_*}

# ----------------------------------------------------------
# 2. False paths: chip register bridge handshake
#    bridge_done and bridge_result come from ARM (100 MHz).
#    bridge_* outputs go to ARM, written in clk_sys.
# ----------------------------------------------------------
set_false_path -to   {emu|minimig|*a2065*|bridge_done}
set_false_path -to   {emu|minimig|*a2065*|bridge_result[*]}
set_false_path -from {emu|minimig|*a2065*|bridge_data[*]}
set_false_path -from {emu|minimig|*a2065*|bridge_addr_off[*]}
set_false_path -from {emu|minimig|*a2065*|bridge_rw}
set_false_path -from {emu|minimig|*a2065*|bridge_new_req}
set_false_path -from {emu|minimig|*a2065*|bridge_done_clr}

# ----------------------------------------------------------
# 3. False paths: static config signals
#    Set once before Amiga boot, never change during
#    normal operation.
# ----------------------------------------------------------
set_false_path -from {emu|minimig|*a2065*|mac_byte*}
set_false_path -from {emu|minimig|*a2065*|a2065_enabled}
set_false_path -from {emu|minimig|*a2065*|arm_int_req}

# ----------------------------------------------------------
# 4. Multicycle paths: boardram registered read
#    a2065_boardram has 1-cycle read latency
#    (addr -> ram_rd register).  Matches existing Minimig
#    pattern for CPU-to-RAM paths: setup 2, hold 1.
# ----------------------------------------------------------
set_multicycle_path -from {emu|minimig|*a2065*|u_boardram|*} \
                    -to   {emu|minimig|*a2065*|u_boardram|ram_rd*} -setup 2
set_multicycle_path -from {emu|minimig|*a2065*|u_boardram|*} \
                    -to   {emu|minimig|*a2065*|u_boardram|ram_rd*} -hold 1

# ----------------------------------------------------------
# 5. Multicycle paths: boardram DTACK generation
#    1-cycle registered DTACK follows the same pattern as
#    other Minimig expansion-card DTACK paths.
# ----------------------------------------------------------
set_multicycle_path -from {emu|amiga_clk|cck*} \
                    -to   {emu|minimig|*a2065*|boardram_dtack_n} -setup 2
set_multicycle_path -from {emu|amiga_clk|cck*} \
                    -to   {emu|minimig|*a2065*|boardram_dtack_n} -hold 1
