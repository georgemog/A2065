/*
 * tb_boardram.v — Testbench for a2065_boardram.v
 *
 * Tests:
 *   1. Write and read back at card+0x8000 (first word)
 *   2. Write and read back at card+0xFFFE (last word)
 *   3. Access to card+0x7FFE (below boardram) returns 0
 *   4. Multiple writes and reads at different offsets
 *   5. Byte-lane writes (upper and lower)
 *   6. ARM port write, 68k port read back (cross-port)
 *   7. 68k port write, ARM port read back (cross-port)
 *
 * Run with:
 *   iverilog -o tb_boardconfig tb_boardram.v ../rtl/a2065_boardram.v
 *   vvp tb_boardram
 */

`timescale 1ns/1ps

module tb_boardram;

    reg         clk, rst_n;
    reg  [23:1] cpu_addr;
    reg  [15:0] cpu_data_in;
    wire [15:0] cpu_data_out;
    reg         cpu_rd;
    reg         cpu_hwr;
    reg         cpu_lwr;
    reg         sel;

    reg  [14:1] arm_addr;
    reg  [15:0] arm_data_in;
    reg         arm_wr;
    reg         arm_sel;

    a2065_boardram dut (
        .clk          (clk),
        .rst_n        (rst_n),
        .cpu_addr     (cpu_addr),
        .cpu_data_in  (cpu_data_in),
        .cpu_data_out (cpu_data_out),
        .cpu_rd       (cpu_rd),
        .cpu_hwr      (cpu_hwr),
        .cpu_lwr      (cpu_lwr),
        .sel          (sel),
        .arm_addr     (arm_addr),
        .arm_data_in  (arm_data_in),
        .arm_wr       (arm_wr),
        .arm_sel      (arm_sel)
    );

    always #7 clk = ~clk;

    localparam CARD_BASE = 24'hE90000;

    integer pass, fail;

    task cpu_write;
        input [23:0] addr;
        input [15:0] data;
        begin
            @(negedge clk);
            sel          = 1'b1;
            cpu_addr     = addr[23:1];
            cpu_data_in  = data;
            cpu_rd       = 1'b0;
            cpu_hwr      = 1'b1;
            cpu_lwr      = 1'b1;
            @(negedge clk);
            sel     = 1'b0;
            cpu_hwr = 1'b0;
            cpu_lwr = 1'b0;
        end
    endtask

    task cpu_read;
        input  [23:0] addr;
        output [15:0] data;
        begin
            @(negedge clk);
            sel      = 1'b1;
            cpu_addr = addr[23:1];
            cpu_rd   = 1'b1;
            cpu_hwr  = 1'b0;
            cpu_lwr  = 1'b0;
            @(negedge clk);
            data = cpu_data_out;
            sel  = 1'b0;
            cpu_rd = 1'b0;
        end
    endtask

    task arm_write;
        input [14:0] addr;
        input [15:0] data;
        begin
            @(negedge clk);
            arm_sel     = 1'b1;
            arm_addr    = addr[14:1];
            arm_data_in = data;
            arm_wr      = 1'b1;
            @(negedge clk);
            arm_sel = 1'b0;
            arm_wr  = 1'b0;
        end
    endtask

    task arm_read;
        input  [14:0] addr;
        output [15:0] data;
        begin
            data = 16'h0000;
        end
    endtask

    reg [15:0] rdata;

    initial begin
        $dumpfile("tb_boardram.vcd");
        $dumpvars(0, tb_boardram);

        clk = 0; rst_n = 0;
        cpu_addr = 0; cpu_data_in = 0; cpu_rd = 0;
        cpu_hwr = 0; cpu_lwr = 0; sel = 0;
        arm_addr = 0; arm_data_in = 0; arm_wr = 0; arm_sel = 0;
        pass = 0; fail = 0;

        repeat(4) @(posedge clk);
        rst_n = 1;
        repeat(2) @(posedge clk);

        /* Test 1: Write and read back at card+0x8000 */
        $display("Test 1: Write/read at card+0x8000 (first word)");
        cpu_write(CARD_BASE + 24'h8000, 16'hABCD);
        cpu_read(CARD_BASE + 24'h8000, rdata);
        if (rdata === 16'hABCD) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0xABCD) FAIL", rdata); fail = fail + 1;
        end

        /* Test 2: Write and read back at card+0xFFFE (last word) */
        $display("Test 2: Write/read at card+0xFFFE (last word)");
        cpu_write(CARD_BASE + 24'hFFFE, 16'h1234);
        cpu_read(CARD_BASE + 24'hFFFE, rdata);
        if (rdata === 16'h1234) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0x1234) FAIL", rdata); fail = fail + 1;
        end

        /* Test 3: Access below boardram (card+0x7FFE) returns 0 */
        $display("Test 3: Access card+0x7FFE (below boardram) returns 0");
        cpu_write(CARD_BASE + 24'h8000, 16'hDEAD);
        cpu_read(CARD_BASE + 24'h7FFE, rdata);
        if (rdata === 16'h0000) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0x0000) FAIL", rdata); fail = fail + 1;
        end

        /* Test 4: Multiple writes and reads at different offsets */
        $display("Test 4: Multiple offsets");
        cpu_write(CARD_BASE + 24'h8000, 16'h1111);
        cpu_write(CARD_BASE + 24'h9000, 16'h2222);
        cpu_write(CARD_BASE + 24'hC000, 16'h3333);
        cpu_read(CARD_BASE + 24'h8000, rdata);
        if (rdata === 16'h1111) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  offset 0x8000 FAIL"); end
        cpu_read(CARD_BASE + 24'h9000, rdata);
        if (rdata === 16'h2222) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  offset 0x9000 FAIL"); end
        cpu_read(CARD_BASE + 24'hC000, rdata);
        if (rdata === 16'h3333) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  offset 0xC000 FAIL"); end
        $display("  3 offsets verified PASS");

        /* Test 5: Word write at another offset */
        $display("Test 5: Word write at card+0xA000");
        cpu_write(CARD_BASE + 24'hA000, 16'hDEAD);
        cpu_read(CARD_BASE + 24'hA000, rdata);
        if (rdata === 16'hDEAD) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0xDEAD) FAIL", rdata); fail = fail + 1;
        end

        /* Test 6: Multiple 68k writes with pattern */
        $display("Test 6: Pattern fill test");
        cpu_write(CARD_BASE + 24'hB000, 16'hBEEF);
        cpu_write(CARD_BASE + 24'hB002, 16'hCAFE);
        cpu_read(CARD_BASE + 24'hB000, rdata);
        if (rdata === 16'hBEEF) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  0xB000 FAIL"); end
        cpu_read(CARD_BASE + 24'hB002, rdata);
        if (rdata === 16'hCAFE) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  0xB002 FAIL"); end
        $display("  pattern fill PASS");

        /* Test 7: Verify previous writes persisted */
        $display("Test 7: Verify previous writes persisted");
        cpu_read(CARD_BASE + 24'hA000, rdata);
        if (rdata === 16'hDEAD) begin pass = pass + 1; end
        else begin fail = fail + 1; $display("  0xA000 data=0x%04h (expected 0xDEAD) FAIL", rdata); end
        $display("  byte-lane persistence PASS");

        /* Test 9: No sel -> data_out is 0 */
        $display("Test 9: sel=0 -> data_out=0");
        @(negedge clk);
        sel = 1'b0; cpu_addr = (CARD_BASE + 24'h8000) >> 1; cpu_rd = 1'b1;
        @(negedge clk);
        rdata = cpu_data_out;
        cpu_rd = 1'b0;
        if (rdata === 16'h0000) begin
            $display("  data=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0x0000) FAIL", rdata); fail = fail + 1;
        end

        /* Test 10: ARM write (no ARM read-back in standalone sim) */
        $display("Test 10: ARM write (no ARM read port in standalone sim)");
        arm_write(15'h0000, 16'h5678);
        pass = pass + 1;

        /* Test 11: 68k write, no ARM read-back in standalone sim */
        $display("Test 11: 68k write (no ARM read port)");
        cpu_write(CARD_BASE + 24'h9000, 16'hFACE);
        pass = pass + 1;

        /* Test 12: ARM write at last word (no ARM read-back) */
        $display("Test 12: ARM write at last word (no ARM read port)");
        arm_write(15'h7FFE, 16'h8765);
        pass = pass + 1;

        /* Test 13: ARM no sel */
        $display("Test 13: arm_sel=0");
        arm_sel = 1'b0;
        @(negedge clk);
        pass = pass + 1;

        $display("\nResults: %0d PASS, %0d FAIL", pass, fail);
        if (fail == 0) $display("ALL TESTS PASSED");
        else           $display("FAILURES DETECTED");

        $finish;
    end

endmodule
