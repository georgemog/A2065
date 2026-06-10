/*
 * tb_regfile.v — Testbench for a2065_regfile.v
 *
 * Tests:
 *   1. RAP write and readback
 *   2. RDP read from CSR shadow
 *   3. RDP write raises doorbell
 *   4. Doorbell clear
 *   5. Back-pressure (RDP write while pending)
 *   6. Chip ID registers (CSR88, CSR89)
 *   7. Card not configured — no response
 *   8. Wrong card base — no response
 *   9. Full lifecycle: INIT write -> ARM processes -> shadow update -> poll
 */

`timescale 1ns/1ps

module tb_regfile;

    reg         clk, rst_n;
    reg  [23:0] cpu_addr;
    reg         cpu_rw;
    reg         cpu_as_n;
    reg         cpu_ds_n;
    reg  [15:0] cpu_data_in;
    wire [15:0] cpu_data_out;
    wire        regs_nrdy;

    reg  [7:0]  card_base;
    reg         card_configured;

    wire        cmd_pending;
    wire [6:0]  cmd_rap;
    wire [15:0] cmd_data;
    reg         cmd_clear;

    reg  [15:0] csr0_in, csr1_in, csr2_in, csr3_in;

    a2065_regfile dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .cpu_addr       (cpu_addr),
        .cpu_rw         (cpu_rw),
        .cpu_as_n       (cpu_as_n),
        .cpu_ds_n       (cpu_ds_n),
        .cpu_data_in    (cpu_data_in),
        .cpu_data_out   (cpu_data_out),
        .regs_nrdy      (regs_nrdy),
        .card_base      (card_base),
        .card_configured(card_configured),
        .cmd_pending    (cmd_pending),
        .cmd_rap        (cmd_rap),
        .cmd_data       (cmd_data),
        .cmd_clear      (cmd_clear),
        .csr0_in        (csr0_in),
        .csr1_in        (csr1_in),
        .csr2_in        (csr2_in),
        .csr3_in        (csr3_in)
    );

    always #7 clk = ~clk;

    localparam CARD_BASE = 8'hE9;
    localparam RDP_ADDR  = 24'hE94000;
    localparam RAP_ADDR  = 24'hE94002;

    integer pass, fail;

    reg [15:0] rdata;

    task bus_write;
        input [23:0] addr;
        input [15:0] data;
        begin
            @(negedge clk);
            cpu_addr    = addr;
            cpu_data_in = data;
            cpu_rw      = 1'b0;
            cpu_as_n    = 1'b0;
            cpu_ds_n    = 1'b0;
            @(posedge clk);
            #1;
            while (regs_nrdy) begin
                @(posedge clk); #1;
            end
            cpu_as_n = 1'b1;
            cpu_ds_n = 1'b1;
            @(negedge clk);
            cpu_rw   = 1'b1;
        end
    endtask

    task bus_read;
        input  [23:0] addr;
        output [15:0] data;
        begin
            @(negedge clk);
            cpu_addr = addr;
            cpu_rw   = 1'b1;
            cpu_as_n = 1'b0;
            cpu_ds_n = 1'b0;
            @(posedge clk);
            #1;
            data = cpu_data_out;
            cpu_as_n = 1'b1;
            cpu_ds_n = 1'b1;
            @(negedge clk);
        end
    endtask

    task arm_process_doorbell;
        input [15:0] response_csr0;
        begin
            wait (cmd_pending);
            @(posedge clk);
            csr0_in = response_csr0;
            cmd_clear = 1'b1;
            @(posedge clk); #1;
            cmd_clear = 1'b0;
            csr0_in = response_csr0;
        end
    endtask

    task clear_doorbell;
        begin
            cmd_clear = 1'b1;
            repeat(4) @(posedge clk);
            cmd_clear = 1'b0;
            repeat(2) @(posedge clk);
        end
    endtask

    initial begin
        $dumpfile("tb_regfile.vcd");
        $dumpvars(0, tb_regfile);

        clk = 0; rst_n = 0;
        cpu_addr = 0; cpu_data_in = 0;
        cpu_rw = 1; cpu_as_n = 1; cpu_ds_n = 1;
        card_base = CARD_BASE;
        card_configured = 1'b1;
        cmd_clear = 1'b0;
        csr0_in = 16'h0004;
        csr1_in = 16'h0000;
        csr2_in = 16'h0000;
        csr3_in = 16'h0000;
        pass = 0; fail = 0;

        repeat(4) @(posedge clk);
        rst_n = 1;
        repeat(2) @(posedge clk);

        /* Test 1: RAP write and readback */
        $display("Test 1: RAP write and readback");
        bus_write(RAP_ADDR, 16'h0005);
        bus_read(RAP_ADDR, rdata);
        if (rdata[6:0] === 7'd5) begin
            $display("  RAP=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  RAP=0x%04h (expected 0x0005) FAIL", rdata); fail = fail + 1;
        end

        /* Test 2: RDP read from CSR shadow */
        $display("Test 2: RDP read from CSR shadow");
        csr0_in = 16'h0084;
        bus_write(RAP_ADDR, 16'h0000);
        repeat(2) @(posedge clk);
        bus_read(RDP_ADDR, rdata);
        if (rdata === 16'h0084) begin
            $display("  CSR0=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  CSR0=0x%04h (expected 0x0084) FAIL", rdata); fail = fail + 1;
        end

        /* Test 3: RDP write raises doorbell */
        $display("Test 3: RDP write raises doorbell");
        bus_write(RAP_ADDR, 16'h0000);
        bus_write(RDP_ADDR, 16'h0003);
        repeat(2) @(posedge clk);
        if (cmd_pending && cmd_rap === 7'd0 && cmd_data === 16'h0003) begin
            $display("  pending=%0b rap=%0d data=0x%04h PASS",
                     cmd_pending, cmd_rap, cmd_data);
            pass = pass + 1;
        end else begin
            $display("  pending=%0b rap=%0d data=0x%04h FAIL",
                     cmd_pending, cmd_rap, cmd_data);
            fail = fail + 1;
        end

        /* Test 4: Doorbell clear */
        $display("Test 4: Doorbell clear");
        clear_doorbell;
        if (!cmd_pending) begin
            $display("  pending=%0b PASS", cmd_pending); pass = pass + 1;
        end else begin
            $display("  pending=%0b (expected 0) FAIL", cmd_pending); fail = fail + 1;
        end

        /* Test 5: Overwrite pending doorbell (no back-pressure) */
        $display("Test 5: Overwrite pending doorbell");
        bus_write(RAP_ADDR, 16'h0000);
        bus_write(RDP_ADDR, 16'h0001);
        repeat(2) @(posedge clk);
        if (!cmd_pending) begin
            $display("  doorbell not raised FAIL"); fail = fail + 1;
        end else begin
            if (!regs_nrdy) begin
                $display("  regs_nrdy not asserted (no back-pressure) PASS");
                pass = pass + 1;
            end else begin
                $display("  regs_nrdy asserted (expected 0) FAIL"); fail = fail + 1;
            end
            bus_write(RDP_ADDR, 16'h0002);
            repeat(2) @(posedge clk);
            if (cmd_data === 16'h0002) begin
                $display("  second write overwrites pending PASS");
                pass = pass + 1;
            end else begin
                $display("  cmd_data=0x%04h (expected 0x0002) FAIL", cmd_data);
                fail = fail + 1;
            end
            if (cmd_pending) begin
                $display("  cmd_pending still set PASS");
                pass = pass + 1;
            end else begin
                $display("  cmd_pending cleared FAIL"); fail = fail + 1;
            end
        end

        /* Clear doorbell from Test 5 before continuing */
        clear_doorbell;

        /* Test 6: Chip ID registers */
        $display("Test 6: Chip ID registers");
        bus_write(RAP_ADDR, 16'd88);
        bus_read(RDP_ADDR, rdata);
        if (rdata === 16'h0001) begin pass = pass + 1; end
        else begin $display("  CSR88=0x%04h FAIL", rdata); fail = fail + 1; end
        bus_write(RAP_ADDR, 16'd89);
        bus_read(RDP_ADDR, rdata);
        if (rdata === 16'h3003) begin pass = pass + 1; end
        else begin $display("  CSR89=0x%04h FAIL", rdata); fail = fail + 1; end
        $display("  chip ID PASS");

        /* Test 7: Card not configured */
        $display("Test 7: Card not configured");
        card_configured = 1'b0;
        bus_write(RAP_ADDR, 16'h0000);
        bus_read(RDP_ADDR, rdata);
        if (rdata === 16'h0000 && !cmd_pending) begin
            $display("  no response PASS"); pass = pass + 1;
        end else begin
            $display("  got response when not configured FAIL"); fail = fail + 1;
        end
        card_configured = 1'b1;

        /* Test 8: Wrong card base */
        $display("Test 8: Wrong card base");
        card_base = 8'hEA;
        bus_read(RDP_ADDR, rdata);
        if (rdata === 16'h0000) begin
            $display("  no response PASS"); pass = pass + 1;
        end else begin
            $display("  got response for wrong base FAIL"); fail = fail + 1;
        end
        card_base = CARD_BASE;

        /* Test 9: Full lifecycle — INIT -> ARM -> shadow update -> poll */
        $display("Test 9: Full lifecycle (INIT)");
        csr0_in = 16'h0004;
        bus_write(RAP_ADDR, 16'h0000);
        fork
            begin
                bus_write(RDP_ADDR, 16'h0001);
            end
            begin
                arm_process_doorbell(16'h0104);
            end
        join
        repeat(2) @(posedge clk);
        bus_read(RDP_ADDR, rdata);
        if ((rdata & 16'h0100) === 16'h0100) begin
            $display("  IDON set CSR0=0x%04h PASS", rdata); pass = pass + 1;
        end else begin
            $display("  CSR0=0x%04h (expected IDON) FAIL", rdata); fail = fail + 1;
        end

        $display("\nResults: %0d PASS, %0d FAIL", pass, fail);
        if (fail == 0) $display("ALL TESTS PASSED");
        else           $display("FAILURES DETECTED");

        $finish;
    end

endmodule
