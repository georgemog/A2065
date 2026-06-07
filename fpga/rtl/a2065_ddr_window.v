/*
 * a2065_ddr_window.v
 *
 * Simulation model for the 68k->DDR3 boardram window.
 *
 * Port A (68k): DTACK-stretched read/write at card+0x8000..0xFFFF.
 * Port B (ARM): instant access (models /dev/mem mmap of same DDR3 region).
 *
 * Three-state FSM: IDLE → BUSY (latency countdown) → DONE (DTACK released)
 * → IDLE (when sel drops).
 *
 * NOTE: Single always block drives ram[] to avoid multiple-driver 'x'.
 */

module a2065_ddr_window #(
    parameter LATENCY = 2
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [23:1] cpu_addr,
    input  wire [15:0] cpu_data_in,
    output reg  [15:0] cpu_data_out,
    input  wire        cpu_hwr,
    input  wire        cpu_lwr,
    input  wire        sel,
    output wire        nrdy,

    input  wire [13:0] arm_addr,
    input  wire [15:0] arm_wdata,
    output wire [15:0] arm_rdata,
    input  wire        arm_wr
);

    reg [15:0] ram [0:16383];

    wire sel_br  = sel && cpu_addr[15];
    wire [13:0] wa = cpu_addr[14:1];
    wire is_write = ~cpu_hwr | ~cpu_lwr;

    localparam S_IDLE = 2'd0;
    localparam S_BUSY = 2'd1;
    localparam S_DONE = 2'd2;

    reg [1:0]  state;
    reg [15:0] cnt;
    reg [13:0] addr_buf;
    reg [15:0] wdata_buf;
    reg        wr_buf;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            cnt          <= 16'd0;
            cpu_data_out <= 16'h0000;
        end else begin
            if (arm_wr)
                ram[arm_addr] <= arm_wdata;

            case (state)
            S_IDLE: begin
                if (sel_br) begin
                    state     <= S_BUSY;
                    cnt       <= LATENCY[15:0];
                    addr_buf  <= wa;
                    wdata_buf <= cpu_data_in;
                    wr_buf    <= is_write;
                end
            end

            S_BUSY: begin
                if (cnt == 16'd0) begin
                    state <= S_DONE;
                    if (wr_buf)
                        ram[addr_buf] <= wdata_buf;
                    else
                        cpu_data_out <= ram[addr_buf];
                end else begin
                    cnt <= cnt - 16'd1;
                end
            end

            S_DONE: begin
                if (!sel_br)
                    state <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

    assign nrdy = (state == S_IDLE) ? sel_br : (state == S_BUSY);

    assign arm_rdata = ram[arm_addr];

endmodule
