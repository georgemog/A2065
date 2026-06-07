/*
 * tb_ddram.v — Testbench for a2065_ddr_window.v
 *
 * Tests:
 *   1. 68k write/read at first word (card+0x8000)
 *   2. 68k write/read at last word (card+0xFFFE)
 *   3. Non-boardram offset (card+0x4000) — no access, nrdy=0
 *   4. ARM write, 68k read back (cross-domain visibility)
 *   5. 68k write, ARM read back (cross-domain visibility)
 *   6. DTACK stretch timing — nrdy asserts during DDR3 latency
 *   7. Back-to-back 68k accesses
 */

`timescale 1ns/1ps

module tb_ddram;

    reg         clk, rst_n;
    reg  [23:1] cpu_addr;
    reg  [15:0] cpu_data_in;
    wire [15:0] cpu_data_out;
    reg         cpu_hwr;
    reg         cpu_lwr;
    reg         sel;
    wire        nrdy;

    reg  [13:0] arm_addr;
    reg  [15:0] arm_wdata;
    wire [15:0] arm_rdata;
    reg         arm_wr;

    a2065_ddr_window #(.LATENCY(2)) dut (
        .clk          (clk),
        .rst_n        (rst_n),
        .cpu_addr     (cpu_addr),
        .cpu_data_in  (cpu_data_in),
        .cpu_data_out (cpu_data_out),
        .cpu_hwr      (cpu_hwr),
        .cpu_lwr      (cpu_lwr),
        .sel          (sel),
        .nrdy         (nrdy),
        .arm_addr     (arm_addr),
        .arm_wdata    (arm_wdata),
        .arm_rdata    (arm_rdata),
        .arm_wr       (arm_wr)
    );

    always #7 clk = ~clk;

    localparam CARD_BASE = 24'hE90000;

    integer pass, fail;
    reg [15:0] rdata;
    integer nrdy_count;

    task cpu_write;
        input [23:0] addr;
        input [15:0] data;
        begin
            @(negedge clk);
            sel          = 1'b1;
            cpu_addr     = addr[23:1];
            cpu_data_in  = data;
            cpu_hwr      = 1'b0;
            cpu_lwr      = 1'b0;
            @(posedge clk);
            while (nrdy) @(posedge clk);
            @(posedge clk);
            sel     = 1'b0;
            cpu_hwr = 1'b1;
            cpu_lwr = 1'b1;
            @(negedge clk);
        end
    endtask

    task cpu_read;
        input  [23:0] addr;
        output [15:0] data;
        begin
            @(negedge clk);
            sel      = 1'b1;
            cpu_addr = addr[23:1];
            cpu_hwr  = 1'b1;
            cpu_lwr  = 1'b1;
            nrdy_count = 0;
            @(posedge clk);
            while (nrdy) begin
                @(posedge clk);
                nrdy_count = nrdy_count + 1;
            end
            @(posedge clk);
            #1;
            data = cpu_data_out;
            sel  = 1'b0;
            @(negedge clk);
        end
    endtask

    task arm_write;
        input [14:0] addr;
        input [15:0] data;
        begin
            arm_addr  = addr[14:1];
            arm_wdata = data;
            arm_wr    = 1'b1;
            @(posedge clk);
            arm_wr = 1'b0;
            @(posedge clk);
        end
    endtask

    task arm_read;
        input  [14:0] addr;
        output [15:0] data;
        begin
            arm_addr = addr[14:1];
            @(posedge clk);
            #1;
            data = arm_rdata;
            @(posedge clk);
        end
    endtask

    initial begin
        $dumpfile("tb_ddram.vcd");
        $dumpvars(0, tb_ddram);

        clk = 0; rst_n = 0;
        cpu_addr = 0; cpu_data_in = 0;
        cpu_hwr = 1; cpu_lwr = 1; sel = 0;
        arm_addr = 0; arm_wdata = 0; arm_wr = 0;
        pass = 0; fail = 0;

        repeat(4) @(posedge clk);
        rst_n = 1;
        repeat(2) @(posedge clk);

        /* Test 1: Write/read at first word (card+0x8000) */
        $display("Test 1: 68k write/read at card+0x8000");
        cpu_write(CARD_BASE + 24'h8000, 16'hABCD);
        cpu_read(CARD_BASE + 24'h8000, rdata);
        if (rdata === 16'hABCD) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0xABCD) FAIL", rdata); fail = fail + 1;
        end

        /* Test 2: Write/read at last word (card+0xFFFE) */
        $display("Test 2: 68k write/read at card+0xFFFE");
        cpu_write(CARD_BASE + 24'hFFFE, 16'h1234);
        cpu_read(CARD_BASE + 24'hFFFE, rdata);
        if (rdata === 16'h1234) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0x1234) FAIL", rdata); fail = fail + 1;
        end

        /* Test 3: Non-boardram offset — nrdy should stay 0 */
        $display("Test 3: Non-boardram offset (card+0x4000)");
        @(negedge clk);
        sel = 1'b1; cpu_addr = (CARD_BASE + 24'h4000) >> 1;
        cpu_hwr = 1'b1; cpu_lwr = 1'b1;
        repeat(4) @(posedge clk);
        if (!nrdy) begin
            $display("  nrdy=0 (no DDR3 access) PASS"); pass = pass + 1;
        end else begin
            $display("  nrdy=1 (unexpected) FAIL"); fail = fail + 1;
        end
        sel = 1'b0;

        /* Test 4: ARM write, 68k read back */
        $display("Test 4: ARM write, 68k read back");
        arm_write(15'h1000, 16'h5678);
        cpu_read(CARD_BASE + 24'h9000, rdata);
        if (rdata === 16'h5678) begin
            $display("  ARM->68k data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  ARM->68k data=0x%04h (expected 0x5678) FAIL", rdata); fail = fail + 1;
        end

        /* Test 5: 68k write, ARM read back */
        $display("Test 5: 68k write, ARM read back");
        cpu_write(CARD_BASE + 24'hA000, 16'hFACE);
        arm_read(15'h2000, rdata);
        if (rdata === 16'hFACE) begin
            $display("  68k->ARM data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  68k->ARM data=0x%04h (expected 0xFACE) FAIL", rdata); fail = fail + 1;
        end

        /* Test 6: DTACK stretch — verify nrdy count matches latency */
        $display("Test 6: DTACK stretch timing");
        cpu_read(CARD_BASE + 24'h8000, rdata);
        if (nrdy_count >= 2) begin
            $display("  nrdy for %0d cycles (>= LATENCY=2) PASS", nrdy_count);
            pass = pass + 1;
        end else begin
            $display("  nrdy for %0d cycles (expected >= 2) FAIL", nrdy_count);
            fail = fail + 1;
        end

        /* Test 7: Back-to-back accesses */
        $display("Test 7: Back-to-back accesses");
        cpu_write(CARD_BASE + 24'h8000, 16'h1111);
        cpu_write(CARD_BASE + 24'h8002, 16'h2222);
        cpu_write(CARD_BASE + 24'h8004, 16'h3333);
        cpu_read(CARD_BASE + 24'h8000, rdata);
        if (rdata === 16'h1111) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  0x8000=0x%04h FAIL", rdata); end
        cpu_read(CARD_BASE + 24'h8002, rdata);
        if (rdata === 16'h2222) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  0x8002=0x%04h FAIL", rdata); end
        cpu_read(CARD_BASE + 24'h8004, rdata);
        if (rdata === 16'h3333) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  0x8004=0x%04h FAIL", rdata); end
        $display("  back-to-back PASS");

        $display("\nResults: %0d PASS, %0d FAIL", pass, fail);
        if (fail == 0) $display("ALL TESTS PASSED");
        else           $display("FAILURES DETECTED");

        $finish;
    end

endmodule
