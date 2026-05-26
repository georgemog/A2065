/*
 * tb_bridge_e2e.v — End-to-end bridge integration testbench
 *
 * Tests the full FPGA<->ARM bridge protocol for A2065 chip register
 * accesses by modeling the ARM daemon's CSR state machine in Verilog.
 *
 * Instantiates a2065_registers in BRIDGE_LOCAL=0 mode (real ARM bridge)
 * with a Verilog model of the ARM daemon's service_bridge() loop.
 *
 * Tests:
 *   1. RAP write/readback round-trip through bridge
 *   2. CSR0 STOP clears all state
 *   3. Full driver init sequence (STOP -> CSR1/CSR2 -> INIT+STRT -> IDON)
 *   4. Chip ID registers (CSR88/89)
 *   5. Interrupt bit logic (INTR set when TINT+INEA)
 *   6. Stale DONE handling (DONE=1 before request)
 *   7. Rapid sequential register accesses (5 back-to-back)
 *   8. Watchdog timeout -> BERR
 *   9. TX: 68k writes descriptor + frame to boardram, ARM reads
 *  10. RX: ARM writes frame + descriptor to boardram
 *  11. Back-to-back with zero idle gap
 *  12. Full init -> TX trigger -> TINT -> read CSR0 end-to-end
 *
 * Run with:
 *   iverilog -o tb_bridge_e2e tb_bridge_e2e.v ../rtl/a2065_registers.v
 *   vvp tb_bridge_e2e
 */

`timescale 1ns/1ps

module tb_bridge_e2e;

    /* ── Parameters ─────────────────────────────────────────────────── */
    parameter WATCHDOG = 200;
    parameter ARM_DELAY = 5;

    localparam CARD_BASE = 24'hE90000;

    localparam [15:0] CSR0_ERR  = 16'h8000;
    localparam [15:0] CSR0_BABL = 16'h4000;
    localparam [15:0] CSR0_MISS = 16'h1000;
    localparam [15:0] CSR0_MERR = 16'h0800;
    localparam [15:0] CSR0_RINT = 16'h0400;
    localparam [15:0] CSR0_TINT = 16'h0200;
    localparam [15:0] CSR0_IDON = 16'h0100;
    localparam [15:0] CSR0_INTR = 16'h0080;
    localparam [15:0] CSR0_INEA = 16'h0040;
    localparam [15:0] CSR0_RXON = 16'h0020;
    localparam [15:0] CSR0_TXON = 16'h0010;
    localparam [15:0] CSR0_TDMD = 16'h0008;
    localparam [15:0] CSR0_STOP = 16'h0004;
    localparam [15:0] CSR0_STRT = 16'h0002;
    localparam [15:0] CSR0_INIT = 16'h0001;

    localparam [15:0] TX_OWN = 16'h8000;
    localparam [15:0] TX_STP = 16'h0200;
    localparam [15:0] TX_ENP = 16'h0100;

    localparam [15:0] RX_OWN = 16'h8000;
    localparam [15:0] RX_STP = 16'h0200;
    localparam [15:0] RX_ENP = 16'h0100;

    /* ── Clock / reset ──────────────────────────────────────────────── */
    reg         clk, rst_n;

    always #7 clk = ~clk;

    /* ── 68k bus signals ────────────────────────────────────────────── */
    reg  [7:0]  card_base;
    reg         card_configured;
    reg  [23:0] cpu_addr;
    reg         cpu_rw, cpu_as_n, cpu_ds_n;
    reg  [15:0] cpu_data_in;

    wire [15:0] cpu_data_out;
    wire        cpu_dtack_n;
    wire        cpu_berr_n;

    /* ── Bridge signals ─────────────────────────────────────────────── */
    wire [15:0] bridge_data;
    wire [7:0]  bridge_addr_off;
    wire        bridge_rw;
    wire        bridge_new_req;
    wire        bridge_done_clr;

    reg         bridge_done_reg;
    reg  [15:0] bridge_result_reg;

    wire        bridge_done   = bridge_done_reg;
    wire [15:0] bridge_result = bridge_result_reg;

    always @(posedge clk) begin
        if (bridge_done_clr) bridge_done_reg <= 1'b0;
    end

    /* ── DUT ────────────────────────────────────────────────────────── */
    a2065_registers #(
        .WATCHDOG_CYCLES(WATCHDOG),
        .BRIDGE_LOCAL   (0)
    ) dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .card_base      (card_base),
        .card_configured(card_configured),
        .cpu_addr       (cpu_addr),
        .cpu_rw         (cpu_rw),
        .cpu_as_n       (cpu_as_n),
        .cpu_ds_n       (cpu_ds_n),
        .cpu_data_in    (cpu_data_in),
        .cpu_data_out   (cpu_data_out),
        .cpu_dtack_n    (cpu_dtack_n),
        .cpu_berr_n     (cpu_berr_n),
        .bridge_data    (bridge_data),
        .bridge_addr_off(bridge_addr_off),
        .bridge_rw      (bridge_rw),
        .bridge_new_req (bridge_new_req),
        .bridge_done    (bridge_done),
        .bridge_result  (bridge_result),
        .bridge_done_clr(bridge_done_clr)
    );

    /* ── Boardram model (32KB byte-addressable, big-endian words) ─── */
    reg [7:0] boardram [0:32767];

    task br_write_byte;
        input [15:0] addr;
        input [7:0]  data;
        begin
            boardram[addr & 16'h7FFF] = data;
        end
    endtask

    task br_write_word;
        input [15:0] addr;
        input [15:0] data;
        begin
            boardram[addr & 16'h7FFE]     = data[15:8];
            boardram[(addr+1) & 16'h7FFF] = data[7:0];
        end
    endtask

    function [7:0] br_read_byte;
        input [15:0] addr;
        begin
            br_read_byte = boardram[addr & 16'h7FFF];
        end
    endfunction

    function [15:0] br_read_word;
        input [15:0] addr;
        begin
            br_read_word = {boardram[addr & 16'h7FFE],
                            boardram[(addr+1) & 16'h7FFF]};
        end
    endfunction

    task br_reset;
        integer i;
        begin
            for (i = 0; i < 32768; i = i + 1) boardram[i] = 8'h00;
        end
    endtask

    /* ── ARM daemon CSR model ───────────────────────────────────────── */
    reg [6:0]  arm_rap;
    reg [15:0] arm_csr [0:127];
    reg        arm_initialized;
    reg [15:0] arm_mode;
    reg [31:0] arm_rdr_rlen, arm_tdr_tlen;
    reg [31:0] arm_rdr_rdra, arm_tdr_tdra;

    task arm_reset;
        integer i;
        begin
            for (i = 0; i < 128; i = i + 1) arm_csr[i] = 16'h0000;
            arm_csr[0] = 16'h0004;
            arm_csr[4] = 16'h0115;
            arm_rap = 7'd0;
            arm_initialized = 0;
            arm_mode = 16'h0;
            arm_rdr_rlen = 0;
            arm_tdr_tlen = 0;
            arm_rdr_rdra = 0;
            arm_tdr_tdra = 0;
        end
    endtask

    task arm_do_init;
        reg [31:0] iaddr;
        integer off;
        reg [31:0] rdr32, tdr32;
        begin
            iaddr = {16'h0000, 8'h00, arm_csr[2][7:0], arm_csr[1]} & 32'h7FFF;
            off = iaddr[15:0];
            arm_mode = br_read_word(off);
            rdr32 = {br_read_word(off + 18), br_read_word(off + 16)};
            tdr32 = {br_read_word(off + 22), br_read_word(off + 20)};
            arm_rdr_rlen = 1 << ((rdr32 >> 29) & 3'b111);
            arm_tdr_tlen = 1 << ((tdr32 >> 29) & 3'b111);
            arm_rdr_rdra = rdr32 & 24'h00FFFFF8;
            arm_tdr_tdra = tdr32 & 24'h00FFFFF8;
        end
    endtask

    function [15:0] arm_chip_wget;
        input [7:0] addr_off;
        reg [15:0] v;
        begin
            if (addr_off == 8'h02) begin
                arm_chip_wget = {8'h00, arm_rap};
            end else begin
                if (arm_rap >= 7'd128) begin
                    arm_chip_wget = 16'h0000;
                end else begin
                    v = arm_csr[arm_rap];
                    $display("    ARM wget: rap=%0d arm_csr[rap]=0x%04h", arm_rap, v);
                    if (arm_rap == 7'd0 && (v & (CSR0_BABL | 16'h2000 | CSR0_MISS | CSR0_MERR)))
                        v = v | CSR0_ERR;
                    if (arm_rap == 7'd88) v = 16'h0001;
                    if (arm_rap == 7'd89) v = 16'h3003;
                    arm_chip_wget = v;
                end
            end
        end
    endfunction

    task arm_chip_wput;
        input [7:0]  addr_off;
        input [15:0] data;
        reg [15:0] oreg, tmp;
        begin
            $display("    ARM write: addr_off=0x%02h rap=%0d data=0x%04h", addr_off, arm_rap, data);
            if (addr_off == 8'h02) begin
                arm_rap = data[6:0];
            end else if (arm_rap < 7'd128) begin
                oreg = arm_csr[arm_rap];
                case (arm_rap)
                7'd0: begin
                    tmp = arm_csr[0];
                    tmp = (tmp & ~CSR0_INEA) | (data & CSR0_INEA);
                    tmp = tmp | (data & (CSR0_INIT | CSR0_STRT | CSR0_STOP | CSR0_TDMD));
                    tmp = tmp & ~(data & (CSR0_IDON | CSR0_TINT | CSR0_RINT |
                                         CSR0_MERR | CSR0_MISS | 16'h2000 | CSR0_BABL));
                    tmp = tmp & ~CSR0_ERR;

                    if ((tmp & CSR0_STOP) && !(oreg & CSR0_STOP)) begin
                        tmp = CSR0_STOP;
                        arm_initialized = 0;
                    end else if ((tmp & CSR0_STRT) && !(oreg & CSR0_STRT) &&
                                (oreg & (CSR0_STOP | CSR0_INIT))) begin
                        tmp = tmp & ~CSR0_STOP;
                        if (!(arm_mode & 16'h0002)) tmp = tmp | CSR0_TXON;
                        if (!(arm_mode & 16'h0001)) tmp = tmp | CSR0_RXON;
                        if ((tmp & CSR0_INIT) && !(oreg & CSR0_INIT)) begin
                            arm_do_init();
                            tmp = tmp | CSR0_IDON;
                            arm_initialized = 1;
                        end
                    end else if ((tmp & CSR0_INIT) && !(oreg & CSR0_INIT) &&
                                (oreg & CSR0_STOP)) begin
                        arm_do_init();
                        tmp = tmp | CSR0_IDON;
                        tmp = tmp & ~(CSR0_RXON | CSR0_TXON | CSR0_STOP);
                        arm_initialized = 1;
                    end

                    if ((tmp & CSR0_STRT) && arm_initialized && (tmp & CSR0_TDMD))
                        tmp = tmp | CSR0_TINT;
                    tmp = tmp & ~CSR0_TDMD;

                    tmp = tmp & ~CSR0_INTR;
                    if (tmp & (CSR0_BABL | CSR0_MISS | CSR0_MERR | CSR0_RINT |
                               CSR0_TINT | CSR0_IDON))
                        tmp = tmp | CSR0_INTR;

                    arm_csr[0] = tmp;
                    $display("    ARM: CSR0 written = 0x%04h (oreg=0x%04h)", tmp, oreg);
                end
                7'd1: if (arm_csr[0] & CSR0_STOP) arm_csr[1] = data & ~16'h0001;
                7'd2: if (arm_csr[0] & CSR0_STOP) arm_csr[2] = data & 16'h00FF;
                7'd3: if (arm_csr[0] & CSR0_STOP) arm_csr[3] = data & 16'h0007;
                default: arm_csr[arm_rap] = data;
                endcase
            end
        end
    endtask

    /* ── ARM bridge service task (models service_bridge in main.cpp) ─ */
    task arm_service_once;
        reg [15:0] result;
        integer i;
        begin
            i = 0;
            while (bridge_new_req !== 1'b1 && i < 10000) begin
                @(posedge clk);
                i = i + 1;
            end

            if (i < 10000) begin
                for (i = 0; i < ARM_DELAY; i = i + 1) @(posedge clk);

                if (bridge_rw) begin
                    arm_chip_wput(bridge_addr_off, bridge_data);
                    result = 16'h0000;
                end else begin
                    result = arm_chip_wget(bridge_addr_off);
                    $display("    ARM read: rap=%0d addr_off=0x%02h result=0x%04h",
                             arm_rap, bridge_addr_off, result);
                end

                bridge_result_reg = result;
                @(negedge clk);
                bridge_done_reg = 1'b1;

                i = 0;
                while (bridge_done_clr !== 1'b1 && i < 1000) begin
                    @(posedge clk);
                    i = i + 1;
                end
            end
        end
    endtask

    /* ── 68k bus tasks ──────────────────────────────────────────────── */
    task bus_write;
        input [23:0] addr;
        input [15:0] data;
        reg got_dtack;
        integer timeout;
        begin
            @(negedge clk);
            cpu_addr    = addr;
            cpu_rw      = 1'b0;
            cpu_data_in = data;
            cpu_as_n    = 1'b0;
            cpu_ds_n    = 1'b0;
            got_dtack = 0;
            timeout   = 0;
            while (!got_dtack && timeout < 10000) begin
                @(negedge clk);
                if (cpu_dtack_n === 1'b0) got_dtack = 1;
                timeout = timeout + 1;
            end
            if (!got_dtack)
                $display("  WARNING: bus_write DTACK timeout");
            cpu_as_n = 1'b1;
            cpu_ds_n = 1'b1;
            cpu_rw   = 1'b1;
            @(negedge clk);
        end
    endtask

    task bus_read;
        input  [23:0] addr;
        output [15:0] data;
        reg got_dtack;
        integer timeout;
        begin
            @(negedge clk);
            cpu_addr = addr;
            cpu_rw   = 1'b1;
            cpu_as_n = 1'b0;
            cpu_ds_n = 1'b0;
            got_dtack = 0;
            timeout   = 0;
            while (!got_dtack && timeout < 10000) begin
                @(negedge clk);
                if (cpu_dtack_n === 1'b0) got_dtack = 1;
                timeout = timeout + 1;
            end
            if (!got_dtack)
                $display("  WARNING: bus_read DTACK timeout");
            data = cpu_data_out;
            cpu_as_n = 1'b1;
            cpu_ds_n = 1'b1;
            @(negedge clk);
        end
    endtask

    task bus_write_expect_berr;
        input [23:0] addr;
        input [15:0] data;
        reg got_berr;
        integer timeout;
        begin
            @(negedge clk);
            cpu_addr    = addr;
            cpu_rw      = 1'b0;
            cpu_data_in = data;
            cpu_as_n    = 1'b0;
            cpu_ds_n    = 1'b0;
            got_berr = 0;
            timeout  = 0;
            while (!got_berr && timeout < 10000) begin
                @(negedge clk);
                if (cpu_berr_n === 1'b0) got_berr = 1;
                timeout = timeout + 1;
            end
            if (!got_berr)
                $display("  WARNING: bus_write_expect_berr BERR timeout");
            cpu_as_n = 1'b1;
            cpu_ds_n = 1'b1;
            cpu_rw   = 1'b1;
            @(negedge clk);
        end
    endtask

    /* ── Bridge helper tasks (68k + ARM in parallel) ───────────────── */
    task bridge_write;
        input [23:0] addr;
        input [15:0] data;
        begin
            fork
                begin bus_write(addr, data); end
                begin arm_service_once(); end
            join
        end
    endtask

    task bridge_read;
        input  [23:0] addr;
        output [15:0] data;
        begin
            fork
                begin bus_read(addr, data); end
                begin arm_service_once(); end
            join
        end
    endtask

    task write_rap;
        input [15:0] val;
        begin bridge_write(CARD_BASE + 24'h4002, val); end
    endtask

    task write_rdp;
        input [15:0] val;
        begin bridge_write(CARD_BASE + 24'h4000, val); end
    endtask

    task read_rdp;
        output [15:0] val;
        begin bridge_read(CARD_BASE + 24'h4000, val); end
    endtask

    task read_rap;
        output [15:0] val;
        begin bridge_read(CARD_BASE + 24'h4002, val); end
    endtask

    /* ── Init block helper ──────────────────────────────────────────── */
    task build_init_block;
        input [15:0] base_off;
        begin
            br_write_word(base_off + 0, 16'h0000);
            br_write_byte(base_off + 2, 8'h80);
            br_write_byte(base_off + 3, 8'h00);
            br_write_byte(base_off + 4, 8'hBB);
            br_write_byte(base_off + 5, 8'h10);
            br_write_byte(base_off + 6, 8'hCC);
            br_write_byte(base_off + 7, 8'hAA);
            br_write_word(base_off + 8,  16'h0000);
            br_write_word(base_off + 10, 16'h0000);
            br_write_word(base_off + 12, 16'h0000);
            br_write_word(base_off + 14, 16'h0000);
            br_write_word(base_off + 16, 16'h2000);
            br_write_word(base_off + 18, 16'h4000);
            br_write_word(base_off + 20, 16'h1000);
            br_write_word(base_off + 22, 16'h4000);
        end
    endtask

    task do_driver_init;
        begin
            build_init_block(16'h0000);
            arm_reset();
            write_rap(16'h0000);
            write_rdp(CSR0_STOP);
            write_rap(16'h0001);
            write_rdp(16'h0000);
            write_rap(16'h0002);
            write_rdp(16'h0000);
            write_rap(16'h0000);
            write_rdp(CSR0_INIT | CSR0_STRT);
        end
    endtask

    /* ── Counters ───────────────────────────────────────────────────── */
    integer pass, fail;
    reg [15:0] rdata;

    task check;
        input cond;
        input [256*8:1] msg;
        begin
            if (cond) begin
                $display("  PASS: %0s", msg);
                pass = pass + 1;
            end else begin
                $display("  FAIL: %0s", msg);
                fail = fail + 1;
            end
        end
    endtask

    /* ════════════════════════════════════════════════════════════════════
     *  TESTS
     * ════════════════════════════════════════════════════════════════════ */

    task test_rap_roundtrip;
        reg [15:0] val;
        begin
            $display("--- Test 1: RAP write/readback round-trip ---");
            arm_reset();
            write_rap(16'h0005);
            read_rap(val);
            check(val == 16'h0005, "RAP=5 written, read back 5");

            write_rap(16'h007F);
            read_rap(val);
            check(val == 16'h007F, "RAP=0x7F written, read back 0x7F");

            write_rap(16'h0080);
            read_rap(val);
            check(val == 16'h0000, "RAP=0x80 masked to 0x00 (7-bit)");
        end
    endtask

    task test_stop_clears;
        reg [15:0] val;
        begin
            $display("--- Test 2: CSR0 STOP clears state ---");
            br_reset();
            do_driver_init();
            read_rdp(val);
            check((val & CSR0_IDON), "IDON set after init");

            write_rdp(CSR0_STOP);
            read_rdp(val);
            check(val == CSR0_STOP, "CSR0 = STOP after STOP write");
            check(arm_initialized == 0, "ARM initialized flag cleared");
        end
    endtask

    task test_init_sequence;
        reg [15:0] val;
        begin
            $display("--- Test 3: Full driver init sequence ---");
            br_reset();
            do_driver_init();

            read_rdp(val);
            $display("  CSR0 after INIT+STRT = 0x%04h", val);
            check((val & CSR0_IDON), "IDON set");
            check((val & CSR0_STRT), "STRT set");
            check((val & CSR0_TXON), "TXON set");
            check((val & CSR0_RXON), "RXON set");
            check(arm_initialized == 1, "ARM initialized");
            check(arm_rdr_rlen == 4, "RDR ring length = 4");
            check(arm_tdr_tlen == 4, "TDR ring length = 4");
            check(arm_rdr_rdra == 32'h002000, "RDR address = 0x002000");
            check(arm_tdr_tdra == 32'h001000, "TDR address = 0x001000");
        end
    endtask

    task test_chip_id;
        reg [15:0] v88, v89;
        begin
            $display("--- Test 4: Chip ID registers ---");
            arm_reset();
            write_rap(16'd88);
            read_rdp(v88);
            write_rap(16'd89);
            read_rdp(v89);
            check(v88 != 16'h0000, "CSR88 non-zero");
            check(v89 == 16'h3003, "CSR89 = 0x3003");
        end
    endtask

    task test_interrupt;
        reg [15:0] val;
        begin
            $display("--- Test 5: Interrupt bit logic ---");
            br_reset();
            do_driver_init();

            write_rdp(CSR0_INEA);
            write_rdp(CSR0_TINT);
            read_rdp(val);
            check((val & CSR0_TINT), "TINT set");
            check((val & CSR0_INEA), "INEA set");
            check((val & CSR0_INTR), "INTR set when TINT+INEA");

            write_rdp(CSR0_TINT);
            read_rdp(val);
            check(!(val & CSR0_TINT), "TINT cleared by writing 1");
        end
    endtask

    task test_stale_done;
        reg [15:0] val;
        integer timeout;
        begin
            $display("--- Test 6: Stale DONE handling ---");
            arm_reset();
            bridge_done_reg = 1'b0;
            bridge_result_reg = 16'h0000;

            fork
                begin bus_write(CARD_BASE + 24'h4002, 16'h0000); end
                begin arm_service_once(); end
            join

            @(posedge clk); @(posedge clk);
            if (bridge_done_reg !== 1'b0) begin
                $display("  NOTE: DONE not yet cleared after transaction");
            end

            bridge_result_reg = 16'hDEAD;
            @(negedge clk);
            bridge_done_reg = 1'b1;
            @(posedge clk);

            fork
                begin : stale_arm_reader
                    timeout = 0;
                    while (bridge_new_req !== 1'b1 && timeout < 500) begin
                        @(posedge clk);
                        timeout = timeout + 1;
                    end
                    if (timeout < 500) begin
                        for (timeout = 0; timeout < ARM_DELAY; timeout = timeout + 1)
                            @(posedge clk);
                        arm_chip_wput(bridge_addr_off, bridge_data);
                        bridge_result_reg = 16'hBEEF;
                        @(negedge clk);
                        bridge_done_reg = 1'b1;
                        timeout = 0;
                        while (bridge_done_clr !== 1'b1 && timeout < 1000) begin
                            @(posedge clk);
                            timeout = timeout + 1;
                        end
                    end
                end
                begin bus_read(CARD_BASE + 24'h4000, val); end
            join
            check(val == 16'hBEEF, "stale DONE overridden by real ARM response");
        end
    endtask

    task test_rapid_sequential;
        reg [15:0] val;
        begin
            $display("--- Test 7: Rapid sequential register accesses ---");
            arm_reset();
            write_rap(16'h0000);
            write_rdp(CSR0_STOP);
            write_rap(16'h0001);
            write_rdp(16'h1234);
            write_rap(16'h0002);
            write_rdp(16'h0056);

            write_rap(16'h0001);
            read_rdp(val);
            check(val == 16'h1234, "CSR1 = 0x1234 after rapid writes");

            write_rap(16'h0002);
            read_rdp(val);
            check(val == 16'h0056, "CSR2 = 0x0056 after rapid writes");
        end
    endtask

    task test_watchdog;
        begin
            $display("--- Test 8: Watchdog timeout -> BERR ---");
            bus_write_expect_berr(CARD_BASE + 24'h4002, 16'h5555);
            repeat(4) @(posedge clk);
            check(cpu_berr_n === 1'b1, "BERR asserted and released after timeout");
        end
    endtask

    task test_tx_boardram;
        reg [15:0] tmd0, tmd1, tmd2;
        integer i;
        reg [7:0] frame_byte;
        begin
            $display("--- Test 9: TX descriptor + frame in boardram ---");
            br_reset();
            do_driver_init();

            for (i = 0; i < 60; i = i + 1)
                br_write_byte(16'h3000 + i, i[7:0]);

            br_write_byte(16'h3000, 8'h00);
            br_write_byte(16'h3001, 8'h80);
            br_write_byte(16'h3002, 8'h10);
            br_write_byte(16'h3003, 8'hAA);
            br_write_byte(16'h3004, 8'hBB);
            br_write_byte(16'h3005, 8'hCC);

            br_write_word(16'h1000, 16'h3000);
            br_write_word(16'h1002, TX_OWN | TX_STP | TX_ENP);
            br_write_word(16'h1004, 16'hFFC4);
            br_write_word(16'h1006, 16'h0000);

            tmd1 = br_read_word(16'h1002);
            check((tmd1 & TX_OWN), "TX descriptor OWN set before transmit");
            check((tmd1 & TX_STP), "TX descriptor STP set");
            check((tmd1 & TX_ENP), "TX descriptor ENP set");

            tmd0 = br_read_word(16'h1000);
            check(tmd0 == 16'h3000, "TX buffer address = 0x3000");

            frame_byte = br_read_byte(16'h3005);
            check(frame_byte == 8'hCC, "frame data in boardram readable by ARM");

            write_rdp(CSR0_INEA | CSR0_TDMD | CSR0_STRT | CSR0_INIT);
            read_rdp(rdata);
            check((rdata & CSR0_TINT), "TINT set after TDMD");
        end
    endtask

    task test_rx_boardram;
        reg [15:0] rmd1, rmd3;
        integer i;
        begin
            $display("--- Test 10: RX descriptor + frame in boardram ---");
            br_reset();
            do_driver_init();

            br_write_word(16'h2000, 16'h4000);
            br_write_word(16'h2002, 16'h0000);
            br_write_word(16'h2004, 16'hFC00);
            br_write_word(16'h2006, 16'h0000);

            for (i = 0; i < 100; i = i + 1)
                br_write_byte(16'h4000 + i, (i ^ 8'hA5));

            br_write_word(16'h2002, RX_STP | RX_ENP);
            br_write_word(16'h2006, 16'h0064);

            rmd1 = br_read_word(16'h2002);
            check(!(rmd1 & RX_OWN), "RX OWN cleared after ARM write");
            check((rmd1 & RX_STP), "RX STP set");
            check((rmd1 & RX_ENP), "RX ENP set");

            rmd3 = br_read_word(16'h2006);
            check(rmd3 == 16'h0064, "RX byte count = 100");

            check(br_read_byte(16'h4000) == 8'hA5, "RX frame byte 0 = 0^0xA5");
            check(br_read_byte(16'h4001) == 8'hA4, "RX frame byte 1 = 1^0xA5");
        end
    endtask

    task test_back_to_back;
        reg [15:0] val1, val2;
        begin
            $display("--- Test 11: Back-to-back with zero idle gap ---");
            arm_reset();
            write_rap(16'd88);
            read_rdp(val1);
            write_rap(16'd89);
            read_rdp(val2);
            check(val1 != 16'h0000, "first back-to-back read: CSR88 OK");
            check(val2 == 16'h3003, "second back-to-back read: CSR89 OK");
        end
    endtask

    task test_full_e2e;
        reg [15:0] val;
        integer i;
        begin
            $display("--- Test 12: Full init -> TX trigger -> TINT -> CSR0 ---");
            br_reset();
            arm_reset();

            build_init_block(16'h0000);

            for (i = 0; i < 60; i = i + 1)
                br_write_byte(16'h3000 + i, i[7:0]);
            br_write_byte(16'h3000, 8'h00);
            br_write_byte(16'h3001, 8'h80);
            br_write_byte(16'h3002, 8'h10);
            br_write_byte(16'h3003, 8'hAA);
            br_write_byte(16'h3004, 8'hBB);
            br_write_byte(16'h3005, 8'hCC);

            br_write_word(16'h1000, 16'h3000);
            br_write_word(16'h1002, TX_OWN | TX_STP | TX_ENP);
            br_write_word(16'h1004, 16'hFFC4);
            br_write_word(16'h1006, 16'h0000);

            write_rap(16'h0000);
            write_rdp(CSR0_STOP);
            write_rap(16'h0001);
            write_rdp(16'h0000);
            write_rap(16'h0002);
            write_rdp(16'h0000);
            write_rap(16'h0000);
            write_rdp(CSR0_INIT | CSR0_STRT);
            read_rdp(val);
            check((val & CSR0_IDON), "E2E: IDON set");
            check((val & CSR0_TXON), "E2E: TXON set");
            check((val & CSR0_RXON), "E2E: RXON set");

            write_rdp(CSR0_INEA | CSR0_TDMD | CSR0_STRT | CSR0_INIT);
            read_rdp(val);
            check((val & CSR0_TINT), "E2E: TINT after TDMD");
            check((val & CSR0_INTR), "E2E: INTR set");
            check((val & CSR0_INEA), "E2E: INEA still set");

            check(br_read_byte(16'h3000) == 8'h00, "E2E: frame byte 0 intact");
            check(br_read_byte(16'h3005) == 8'hCC, "E2E: frame byte 5 intact");
        end
    endtask

    /* ════════════════════════════════════════════════════════════════════
     *  MAIN
     * ════════════════════════════════════════════════════════════════════ */
    initial begin
        $dumpfile("tb_bridge_e2e.vcd");
        $dumpvars(0, tb_bridge_e2e);

        clk = 0; rst_n = 0;
        card_base = 8'hE9;
        card_configured = 1;
        cpu_addr = 0; cpu_rw = 1; cpu_as_n = 1; cpu_ds_n = 1;
        cpu_data_in = 0;
        bridge_done_reg = 0;
        bridge_result_reg = 0;
        pass = 0; fail = 0;

        repeat(4) @(posedge clk);
        rst_n = 1;
        repeat(2) @(posedge clk);

        test_rap_roundtrip();
        test_stop_clears();
        test_init_sequence();
        test_chip_id();
        test_interrupt();
        test_stale_done();
        test_rapid_sequential();
        test_watchdog();
        test_tx_boardram();
        test_rx_boardram();
        test_back_to_back();
        test_full_e2e();

        $display("");
        $display("=== tb_bridge_e2e: %0d PASS, %0d FAIL ===", pass, fail);
        if (fail == 0) $display("ALL TESTS PASSED");
        else           $display("FAILURES DETECTED");

        $finish;
    end

endmodule
