/*
 * tb_autoconfig.v — Testbench for a2065_autoconfig.v
 *
 * Simulates AmigaOS autoconfig read sequence:
 *   1. Read 32 nibbles from 0xE80000..0xE8003E
 *   2. Write base address to 0xE80048
 *   3. Write SHUTUP to 0xE8004C
 *   4. Verify card_configured, card_base, card_shutup
 *
 * Run with:
 *   iverilog -o tb_autoconfig tb_autoconfig.v ../rtl/a2065_autoconfig.v
 *   vvp tb_autoconfig
 */

`timescale 1ns/1ps

module tb_autoconfig;

    reg        clk, rst_n;
    reg [23:0] cpu_addr;
    reg        cpu_rw, cpu_as_n;
    reg [15:0] cpu_data_in;

    wire [15:0] cpu_data_out;
    wire        cpu_dtack_n;
    wire [7:0]  card_base;
    wire        card_configured, card_shutup;

    a2065_autoconfig dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .cpu_addr       (cpu_addr),
        .cpu_rw         (cpu_rw),
        .cpu_as_n       (cpu_as_n),
        .cpu_data_in    (cpu_data_in),
        .cpu_data_out   (cpu_data_out),
        .cpu_dtack_n    (cpu_dtack_n),
        .mac_byte2      (8'h42),
        .mac_byte3      (8'hAB),
        .mac_byte4      (8'hCD),
        .mac_byte5      (8'hEF),
        .card_base      (card_base),
        .card_configured(card_configured),
        .card_shutup    (card_shutup)
    );

    always #7 clk = ~clk; /* ~71MHz */

    /* Expected nibbles (inverted) for A2065 */
    /* MAC bytes: 2=0x42, 3=0xAB, 4=0xCD, 5=0xEF */
    reg [3:0] expected_nibbles [0:31];
    initial begin
        expected_nibbles[ 0] = ~4'hC;
        expected_nibbles[ 1] = ~4'h1;
        expected_nibbles[ 2] = ~4'h7;
        expected_nibbles[ 3] = ~4'h0;
        expected_nibbles[ 4] = ~4'h0;
        expected_nibbles[ 5] = ~4'h0;
        expected_nibbles[ 6] = ~4'hF; /* reserved → inverted 0xF = 0x0 */
        expected_nibbles[ 7] = ~4'hF;
        expected_nibbles[ 8] = ~4'h0;
        expected_nibbles[ 9] = ~4'h2;
        expected_nibbles[10] = ~4'h0;
        expected_nibbles[11] = ~4'h2;
        /* MAC byte 2 = 0x42 */
        expected_nibbles[12] = ~4'h4;
        expected_nibbles[13] = ~4'h2;
        /* MAC byte 3 = 0xAB */
        expected_nibbles[14] = ~4'hA;
        expected_nibbles[15] = ~4'hB;
        /* MAC byte 4 = 0xCD */
        expected_nibbles[16] = ~4'hC;
        expected_nibbles[17] = ~4'hD;
        /* MAC byte 5 = 0xEF */
        expected_nibbles[18] = ~4'hE;
        expected_nibbles[19] = ~4'hF;
        /* rest = inverted 0xF */
        expected_nibbles[20] = ~4'hF;
        expected_nibbles[21] = ~4'hF;
        expected_nibbles[22] = ~4'hF;
        expected_nibbles[23] = ~4'hF;
        expected_nibbles[24] = ~4'hF;
        expected_nibbles[25] = ~4'hF;
        expected_nibbles[26] = ~4'hF;
        expected_nibbles[27] = ~4'hF;
        expected_nibbles[28] = ~4'hF;
        expected_nibbles[29] = ~4'hF;
        expected_nibbles[30] = ~4'hF;
        expected_nibbles[31] = ~4'hF;
    end

    integer i, pass, fail;
    reg [3:0] got_nibble;

    task bus_read;
        input [23:0] addr;
        output [15:0] data;
        reg got_dtack;
        integer timeout;
        begin
            @(negedge clk);
            cpu_addr  = addr;
            cpu_rw    = 1'b1;
            cpu_as_n  = 1'b0;
            got_dtack = 0;
            timeout   = 0;
            while (!got_dtack && timeout < 1000) begin
                @(negedge clk);
                if (cpu_dtack_n === 1'b0) got_dtack = 1;
                timeout = timeout + 1;
            end
            data = cpu_data_out;
            cpu_as_n = 1'b1;
            cpu_rw   = 1'b1;
        end
    endtask

    task bus_write;
        input [23:0] addr;
        input [15:0] data;
        begin
            @(negedge clk);
            cpu_addr     = addr;
            cpu_rw       = 1'b0;
            cpu_data_in  = data;
            cpu_as_n     = 1'b0;
            @(negedge cpu_dtack_n);
            @(posedge clk);
            cpu_as_n = 1'b1;
            cpu_rw   = 1'b1;
        end
    endtask

    reg [15:0] rdata;

    initial begin
        $dumpfile("tb_autoconfig.vcd");
        $dumpvars(0, tb_autoconfig);

        clk = 0; rst_n = 0;
        cpu_addr = 0; cpu_rw = 1; cpu_as_n = 1; cpu_data_in = 0;
        pass = 0; fail = 0;

        repeat(4) @(posedge clk);
        rst_n = 1;
        repeat(2) @(posedge clk);

        /* Test 1: Read all 32 autoconfig nibbles */
        $display("Test 1: Autoconfig nibble read sequence");
        for (i = 0; i < 32; i = i + 1) begin
            bus_read(24'hE80000 + i*2, rdata);
            got_nibble = rdata[15:12];
            if (got_nibble === expected_nibbles[i]) begin
                $display("  nibble[%0d] = %h (expected %h) PASS", i, got_nibble, expected_nibbles[i]);
                pass = pass + 1;
            end else begin
                $display("  nibble[%0d] = %h (expected %h) FAIL", i, got_nibble, expected_nibbles[i]);
                fail = fail + 1;
            end
        end

        /* Test 2: card_configured not set yet */
        if (card_configured === 1'b0) begin
            $display("Test 2: card_configured=0 before base write PASS");
            pass = pass + 1;
        end else begin
            $display("Test 2: card_configured should be 0 FAIL");
            fail = fail + 1;
        end

        /* Test 3: Write base address */
        $display("Test 3: Write base address 0xE9 to 0xE80048");
        bus_write(24'hE80048, 16'hE900);
        @(posedge clk); @(posedge clk);
        if (card_configured === 1'b1 && card_base === 8'hE9) begin
            $display("  card_configured=1 card_base=0x%02h PASS", card_base);
            pass = pass + 1;
        end else begin
            $display("  card_configured=%b card_base=0x%02h FAIL", card_configured, card_base);
            fail = fail + 1;
        end

        /* Test 4: SHUTUP */
        $display("Test 4: Write SHUTUP to 0xE8004C");
        bus_write(24'hE8004C, 16'h0000);
        @(posedge clk); @(posedge clk);
        if (card_shutup === 1'b1) begin
            $display("  card_shutup=1 PASS"); pass = pass + 1;
        end else begin
            $display("  card_shutup should be 1 FAIL"); fail = fail + 1;
        end

        /* Test 5: No response after SHUTUP */
        $display("Test 5: No response after SHUTUP");
        bus_read(24'hE80000, rdata);
        if (rdata === 16'hFFFF) begin
            $display("  data=0xFFFF (no response) PASS"); pass = pass + 1;
        end else begin
            $display("  data=0x%04h (expected 0xFFFF) FAIL", rdata); fail = fail + 1;
        end

        $display("\nResults: %0d PASS, %0d FAIL", pass, fail);
        if (fail == 0) $display("ALL TESTS PASSED");
        else           $display("FAILURES DETECTED");

        $finish;
    end

endmodule
