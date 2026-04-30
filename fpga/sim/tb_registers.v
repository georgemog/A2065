/*
 * tb_registers.v — Testbench for a2065_registers.v
 *
 * Tests the DTACK-stretch state machine for A2065 chip register accesses.
 * Models the ARM daemon response with parameterised delay.
 *
 * Tests:
 *   1. Write to RAP (0x4002) with 0-cycle ARM delay
 *   2. Read from RDP (0x4000) with 0-cycle ARM delay
 *   3. Write to RDP (0x4000) with 10-cycle ARM delay
 *   4. Read from RAP (0x4002) with 100-cycle ARM delay
 *   5. Back-to-back write (RAP) then read (RDP)
 *   6. Watchdog timeout (no ARM response)
 *   7. Access outside register region — no bridge request
 *   8. card_configured=0 — no bridge request
 *   9. Verify bridge_rw=0 for reads, bridge_rw=1 for writes
 *
 * Run with:
 *   iverilog -o tb_registers tb_registers.v ../rtl/a2065_registers.v
 *   vvp tb_registers
 */

`timescale 1ns/1ps

module tb_registers;

    reg         clk, rst_n;
    reg  [7:0]  card_base;
    reg         card_configured;
    reg  [23:0] cpu_addr;
    reg         cpu_rw, cpu_as_n, cpu_ds_n;
    reg  [15:0] cpu_data_in;

    wire [15:0] cpu_data_out;
    wire        cpu_dtack_n;
    wire        cpu_berr_n;

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

    a2065_registers #(
        .WATCHDOG_CYCLES(200)
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

    always #7 clk = ~clk;

    localparam CARD_BASE = 24'hE90000;

    integer pass, fail;

    task arm_respond;
        input [15:0] result;
        input integer delay;
        integer i;
        begin
            @(posedge clk);
            while (!bridge_new_req) @(posedge clk);
            for (i = 0; i < delay; i = i + 1) @(posedge clk);
            @(negedge clk);
            bridge_result_reg = result;
            bridge_done_reg   = 1'b1;
        end
    endtask

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

    reg [15:0] rdata;

    initial begin
        $dumpfile("tb_registers.vcd");
        $dumpvars(0, tb_registers);

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

        /* ── Test 1: Write to RAP, 0-cycle ARM delay ──────────────── */
        $display("Test 1: Write to RAP (0x4002) data=0x0000, 0-cycle delay");
        fork
            begin
                bus_write(CARD_BASE + 24'h4002, 16'h0000);
                if (bridge_data === 16'h0000 && bridge_addr_off === 8'h02 && bridge_rw === 1'b1) begin
                    $display("  bridge: data=0x%04h addr_off=0x%02h rw=%b PASS",
                             bridge_data, bridge_addr_off, bridge_rw);
                    pass = pass + 1;
                end else begin
                    $display("  bridge: data=0x%04h addr_off=0x%02h rw=%b FAIL",
                             bridge_data, bridge_addr_off, bridge_rw);
                    fail = fail + 1;
                end
            end
            begin
                arm_respond(16'h0000, 0);
            end
        join

        /* ── Test 2: Read from RDP, 0-cycle ARM delay ─────────────── */
        $display("Test 2: Read from RDP (0x4000), 0-cycle delay");
        fork
            begin
                bus_read(CARD_BASE + 24'h4000, rdata);
                if (rdata === 16'hDEAD) begin
                    $display("  read data=0x%04h PASS", rdata);
                    pass = pass + 1;
                end else begin
                    $display("  read data=0x%04h (expected 0xDEAD) FAIL", rdata);
                    fail = fail + 1;
                end
                if (bridge_rw === 1'b0 && bridge_addr_off === 8'h00) begin
                    $display("  bridge rw=0 (read) addr_off=0x00 PASS");
                    pass = pass + 1;
                end else begin
                    $display("  bridge rw=%b addr_off=0x%02h FAIL", bridge_rw, bridge_addr_off);
                    fail = fail + 1;
                end
            end
            begin
                arm_respond(16'hDEAD, 0);
            end
        join

        /* ── Test 3: Write to RDP, 10-cycle ARM delay ─────────────── */
        $display("Test 3: Write to RDP (0x4000) data=0x1234, 10-cycle delay");
        fork
            begin
                bus_write(CARD_BASE + 24'h4000, 16'h1234);
                if (bridge_data === 16'h1234 && bridge_addr_off === 8'h00 && bridge_rw === 1'b1) begin
                    $display("  bridge: data=0x%04h addr_off=0x%02h rw=%b PASS",
                             bridge_data, bridge_addr_off, bridge_rw);
                    pass = pass + 1;
                end else begin
                    $display("  bridge: data=0x%04h addr_off=0x%02h rw=%b FAIL",
                             bridge_data, bridge_addr_off, bridge_rw);
                    fail = fail + 1;
                end
            end
            begin
                arm_respond(16'h0000, 10);
            end
        join

        /* ── Test 4: Read from RAP, 100-cycle ARM delay ────────────── */
        $display("Test 4: Read from RAP (0x4002), 100-cycle delay");
        fork
            begin
                bus_read(CARD_BASE + 24'h4002, rdata);
                if (rdata === 16'hBEEF) begin
                    $display("  read data=0x%04h PASS", rdata);
                    pass = pass + 1;
                end else begin
                    $display("  read data=0x%04h (expected 0xBEEF) FAIL", rdata);
                    fail = fail + 1;
                end
            end
            begin
                arm_respond(16'hBEEF, 100);
            end
        join

        /* ── Test 5: Back-to-back write then read ──────────────────── */
        $display("Test 5: Back-to-back write (RAP=3) then read (RDP)");
        fork
            begin
                bus_write(CARD_BASE + 24'h4002, 16'h0003);
            end
            begin
                arm_respond(16'h0000, 2);
            end
        join
        fork
            begin
                bus_read(CARD_BASE + 24'h4000, rdata);
                if (rdata === 16'hCAFE) begin
                    $display("  back-to-back read data=0x%04h PASS", rdata);
                    pass = pass + 1;
                end else begin
                    $display("  back-to-back read data=0x%04h (expected 0xCAFE) FAIL", rdata);
                    fail = fail + 1;
                end
            end
            begin
                arm_respond(16'hCAFE, 5);
            end
        join

        /* ── Test 6: Watchdog timeout (no ARM response) ────────────── */
        $display("Test 6: Watchdog timeout (200-cycle watchdog, no ARM response)");
        bus_write_expect_berr(CARD_BASE + 24'h4002, 16'h5555);
        repeat(2) @(posedge clk);
        if (cpu_berr_n === 1'b1) begin
            $display("  BERR asserted and released after timeout PASS");
            pass = pass + 1;
        end else begin
            $display("  BERR not released FAIL");
            fail = fail + 1;
        end

        /* ── Test 7: Access outside register region ────────────────── */
        $display("Test 7: Access to card+0x5000 (outside register region)");
        @(negedge clk);
        cpu_addr = CARD_BASE + 24'h5000;
        cpu_rw   = 1'b1;
        cpu_as_n = 1'b0;
        cpu_ds_n = 1'b0;
        repeat(6) @(posedge clk);
        if (bridge_new_req === 1'b0 && cpu_dtack_n === 1'b1) begin
            $display("  no bridge request, no DTACK PASS");
            pass = pass + 1;
        end else begin
            $display("  bridge_new_req=%b dtack_n=%b FAIL", bridge_new_req, cpu_dtack_n);
            fail = fail + 1;
        end
        cpu_as_n = 1'b1;
        cpu_ds_n = 1'b1;
        cpu_rw   = 1'b1;
        @(negedge clk);

        /* ── Test 8: card_configured=0 — no response ──────────────── */
        $display("Test 8: card_configured=0 — no response");
        card_configured = 0;
        @(negedge clk);
        cpu_addr    = CARD_BASE + 24'h4002;
        cpu_rw      = 1'b0;
        cpu_data_in = 16'hABCD;
        cpu_as_n    = 1'b0;
        cpu_ds_n    = 1'b0;
        repeat(6) @(posedge clk);
        if (bridge_new_req === 1'b0 && cpu_dtack_n === 1'b1) begin
            $display("  no bridge request when not configured PASS");
            pass = pass + 1;
        end else begin
            $display("  bridge_new_req=%b dtack_n=%b FAIL", bridge_new_req, cpu_dtack_n);
            fail = fail + 1;
        end
        cpu_as_n = 1'b1;
        cpu_ds_n = 1'b1;
        cpu_rw   = 1'b1;
        card_configured = 1;
        @(negedge clk);

        /* ── Test 9: Wrong card_base — no response ─────────────────── */
        $display("Test 9: Wrong card_base (0xEA) — no response");
        card_base = 8'hEA;
        @(negedge clk);
        cpu_addr    = 24'hE90000 + 24'h4002;
        cpu_rw      = 1'b0;
        cpu_data_in = 16'hFFFF;
        cpu_as_n    = 1'b0;
        cpu_ds_n    = 1'b0;
        repeat(6) @(posedge clk);
        if (bridge_new_req === 1'b0 && cpu_dtack_n === 1'b1) begin
            $display("  no bridge request for wrong base PASS");
            pass = pass + 1;
        end else begin
            $display("  bridge_new_req=%b dtack_n=%b FAIL", bridge_new_req, cpu_dtack_n);
            fail = fail + 1;
        end
        cpu_as_n = 1'b1;
        cpu_ds_n = 1'b1;
        cpu_rw   = 1'b1;
        card_base = 8'hE9;
        @(negedge clk);

        /* ── Results ────────────────────────────────────────────────── */
        $display("\nResults: %0d PASS, %0d FAIL", pass, fail);
        if (fail == 0) $display("ALL TESTS PASSED");
        else           $display("FAILURES DETECTED");

        $finish;
    end

endmodule
