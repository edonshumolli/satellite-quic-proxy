# Timing constraints for the QUIC Accelerator design
# For Xilinx Vivado

# Clock definition
create_clock -period 10.000 -name sys_clk -waveform {0.000 5.000} [get_ports sys_clk]

# Input and output delay constraints
set_input_delay -clock sys_clk -max 2.000 [get_ports {s_axis_tdata[*]}]
set_input_delay -clock sys_clk -max 2.000 [get_ports {s_axis_tkeep[*]}]
set_input_delay -clock sys_clk -max 2.000 [get_ports {s_axis_tuser[*]}]
set_input_delay -clock sys_clk -max 2.000 [get_ports s_axis_tvalid]
set_input_delay -clock sys_clk -max 2.000 [get_ports s_axis_tlast]
set_input_delay -clock sys_clk -max 2.000 [get_ports m_axis_tready]

set_output_delay -clock sys_clk -max 2.000 [get_ports {m_axis_tdata[*]}]
set_output_delay -clock sys_clk -max 2.000 [get_ports {m_axis_tkeep[*]}]
set_output_delay -clock sys_clk -max 2.000 [get_ports {m_axis_tuser[*]}]
set_output_delay -clock sys_clk -max 2.000 [get_ports m_axis_tvalid]
set_output_delay -clock sys_clk -max 2.000 [get_ports m_axis_tlast]
set_output_delay -clock sys_clk -max 2.000 [get_ports s_axis_tready]

# Register interface timing
set_input_delay -clock sys_clk -max 2.000 [get_ports {reg_addr[*]}]
set_input_delay -clock sys_clk -max 2.000 [get_ports {reg_wr_data[*]}]
set_input_delay -clock sys_clk -max 2.000 [get_ports reg_wr_en]
set_input_delay -clock sys_clk -max 2.000 [get_ports reg_rd_en]

set_output_delay -clock sys_clk -max 2.000 [get_ports {reg_rd_data[*]}]
set_output_delay -clock sys_clk -max 2.000 [get_ports reg_rd_valid]

# False path constraints
set_false_path -from [get_ports sys_rst_n]
set_false_path -to [get_ports irq]
set_false_path -to [get_ports {status_leds[*]}]

# Clock crossing constraints
# (These would be needed for multi-clock designs)

# Path groups for better timing analysis
create_pblock pblock_crypto
add_cells_to_pblock [get_pblocks pblock_crypto] [get_cells crypto_inst]
resize_pblock [get_pblocks pblock_crypto] -add {SLICE_X0Y0:SLICE_X50Y50}

create_pblock pblock_compression
add_cells_to_pblock [get_pblocks pblock_compression] [get_cells comp_inst]
resize_pblock [get_pblocks pblock_compression] -add {SLICE_X51Y0:SLICE_X100Y50}

create_pblock pblock_packet
add_cells_to_pblock [get_pblocks pblock_packet] [get_cells pkt_inst]
resize_pblock [get_pblocks pblock_packet] -add {SLICE_X0Y51:SLICE_X50Y100}

# Max delay constraints for critical paths
set_max_delay 9.000 -from [get_cells crypto_inst/*] -to [get_cells axi_interface/*]
set_max_delay 9.000 -from [get_cells comp_inst/*] -to [get_cells axi_interface/*]
set_max_delay 9.000 -from [get_cells pkt_inst/*] -to [get_cells axi_interface/*]

# Power optimization
set_power_opt -exclude_cells [get_cells crypto_inst/aes_inst]
