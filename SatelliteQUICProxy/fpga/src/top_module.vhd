----------------------------------------------------------------------------------
-- Top Module for QUIC Hardware Acceleration
-- 
-- This module integrates all the QUIC hardware acceleration components:
-- - QUIC Crypto for encryption/decryption
-- - QUIC Compression for data compression/decompression
-- - QUIC Packet Processor for packet framing and handling
-- - AXI Stream Interface for host communication
--
-- The module provides a comprehensive FPGA-based accelerator for QUIC protocol
-- operations that can be integrated with a host system via PCIe or other
-- high-speed interfaces.
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity quic_accelerator_top is
    Port (
        -- Clock and reset
        sys_clk         : in  STD_LOGIC;  -- System clock
        sys_rst_n       : in  STD_LOGIC;  -- Active low reset
        
        -- AXI Stream slave interface (from host)
        s_axis_tdata    : in  STD_LOGIC_VECTOR(63 downto 0);
        s_axis_tkeep    : in  STD_LOGIC_VECTOR(7 downto 0);
        s_axis_tuser    : in  STD_LOGIC_VECTOR(7 downto 0);
        s_axis_tvalid   : in  STD_LOGIC;
        s_axis_tlast    : in  STD_LOGIC;
        s_axis_tready   : out STD_LOGIC;
        
        -- AXI Stream master interface (to host)
        m_axis_tdata    : out STD_LOGIC_VECTOR(63 downto 0);
        m_axis_tkeep    : out STD_LOGIC_VECTOR(7 downto 0);
        m_axis_tuser    : out STD_LOGIC_VECTOR(7 downto 0);
        m_axis_tvalid   : out STD_LOGIC;
        m_axis_tlast    : out STD_LOGIC;
        m_axis_tready   : in  STD_LOGIC;
        
        -- Memory-mapped register interface (AXI Lite or custom)
        reg_addr        : in  STD_LOGIC_VECTOR(15 downto 0);
        reg_wr_data     : in  STD_LOGIC_VECTOR(31 downto 0);
        reg_rd_data     : out STD_LOGIC_VECTOR(31 downto 0);
        reg_wr_en       : in  STD_LOGIC;
        reg_rd_en       : in  STD_LOGIC;
        reg_rd_valid    : out STD_LOGIC;
        
        -- Interrupt signals
        irq             : out STD_LOGIC;
        
        -- Status signals
        status_leds     : out STD_LOGIC_VECTOR(3 downto 0)
    );
end quic_accelerator_top;

architecture Behavioral of quic_accelerator_top is
    -- Internal clock and reset
    signal clk          : STD_LOGIC;
    signal rst          : STD_LOGIC;
    signal rst_n        : STD_LOGIC;
    
    -- Control registers
    signal ctrl_reg_0   : STD_LOGIC_VECTOR(31 downto 0);  -- Mode control
    signal ctrl_reg_1   : STD_LOGIC_VECTOR(31 downto 0);  -- Connection parameters
    signal ctrl_reg_2   : STD_LOGIC_VECTOR(31 downto 0);  -- Packet parameters
    signal ctrl_reg_3   : STD_LOGIC_VECTOR(31 downto 0);  -- Crypto parameters
    
    -- Status registers
    signal status_reg_0 : STD_LOGIC_VECTOR(31 downto 0);  -- General status
    signal status_reg_1 : STD_LOGIC_VECTOR(31 downto 0);  -- Crypto status
    signal status_reg_2 : STD_LOGIC_VECTOR(31 downto 0);  -- Compression status
    signal status_reg_3 : STD_LOGIC_VECTOR(31 downto 0);  -- Packet processor status
    
    -- Control signals derived from registers
    signal module_select: STD_LOGIC_VECTOR(3 downto 0);   -- Which module to use
    signal operation_mode: STD_LOGIC_VECTOR(3 downto 0);  -- Operation mode
    signal bypass_enable: STD_LOGIC;                      -- Bypass acceleration
    signal start_process: STD_LOGIC;                      -- Start processing
    
    -- Module interconnect signals
    -- Crypto module connections
    signal crypto_data_in      : STD_LOGIC_VECTOR(63 downto 0);
    signal crypto_valid_in     : STD_LOGIC;
    signal crypto_last_in      : STD_LOGIC;
    signal crypto_ready_in     : STD_LOGIC;
    signal crypto_data_out     : STD_LOGIC_VECTOR(63 downto 0);
    signal crypto_valid_out    : STD_LOGIC;
    signal crypto_last_out     : STD_LOGIC;
    signal crypto_ready_out    : STD_LOGIC;
    
    -- Compression module connections
    signal comp_data_in        : STD_LOGIC_VECTOR(63 downto 0);
    signal comp_valid_in       : STD_LOGIC;
    signal comp_last_in        : STD_LOGIC;
    signal comp_ready_in       : STD_LOGIC;
    signal comp_data_out       : STD_LOGIC_VECTOR(63 downto 0);
    signal comp_valid_out      : STD_LOGIC;
    signal comp_last_out       : STD_LOGIC;
    signal comp_ready_out      : STD_LOGIC;
    
    -- Packet processor connections
    signal pkt_data_in         : STD_LOGIC_VECTOR(63 downto 0);
    signal pkt_valid_in        : STD_LOGIC;
    signal pkt_last_in         : STD_LOGIC;
    signal pkt_ready_in        : STD_LOGIC;
    signal pkt_data_out        : STD_LOGIC_VECTOR(63 downto 0);
    signal pkt_valid_out       : STD_LOGIC;
    signal pkt_last_out        : STD_LOGIC;
    signal pkt_ready_out       : STD_LOGIC;
    
    -- Common interface signals
    signal quic_data_in        : STD_LOGIC_VECTOR(63 downto 0);
    signal quic_valid_in       : STD_LOGIC;
    signal quic_last_in        : STD_LOGIC;
    signal quic_ready_in       : STD_LOGIC;
    signal quic_data_out       : STD_LOGIC_VECTOR(63 downto 0);
    signal quic_valid_out      : STD_LOGIC;
    signal quic_last_out       : STD_LOGIC;
    signal quic_ready_out      : STD_LOGIC;
    
    -- Module status and control
    signal crypto_done         : STD_LOGIC;
    signal comp_done           : STD_LOGIC;
    signal pkt_done            : STD_LOGIC;
    signal quic_status         : STD_LOGIC_VECTOR(31 downto 0);
    
    -- Component declarations
    component quic_crypto is
        Port (
            clk             : in  STD_LOGIC;
            rst             : in  STD_LOGIC;
            start           : in  STD_LOGIC;
            encrypt         : in  STD_LOGIC;
            done            : out STD_LOGIC;
            data_in         : in  STD_LOGIC_VECTOR(127 downto 0);
            data_in_valid   : in  STD_LOGIC;
            data_in_ready   : out STD_LOGIC;
            data_out        : out STD_LOGIC_VECTOR(127 downto 0);
            data_out_valid  : out STD_LOGIC;
            data_out_ready  : in  STD_LOGIC;
            key             : in  STD_LOGIC_VECTOR(127 downto 0);
            nonce           : in  STD_LOGIC_VECTOR(95 downto 0);
            aad             : in  STD_LOGIC_VECTOR(127 downto 0);
            aad_valid       : in  STD_LOGIC;
            aad_ready       : out STD_LOGIC;
            auth_tag_in     : in  STD_LOGIC_VECTOR(127 downto 0);
            auth_tag_out    : out STD_LOGIC_VECTOR(127 downto 0);
            auth_result     : out STD_LOGIC
        );
    end component;
    
    component quic_compression is
        Port (
            clk               : in  STD_LOGIC;
            rst               : in  STD_LOGIC;
            start             : in  STD_LOGIC;
            compress          : in  STD_LOGIC;
            done              : out STD_LOGIC;
            data_in           : in  STD_LOGIC_VECTOR(63 downto 0);
            data_in_valid     : in  STD_LOGIC;
            data_in_last      : in  STD_LOGIC;
            data_in_ready     : out STD_LOGIC;
            data_out          : out STD_LOGIC_VECTOR(63 downto 0);
            data_out_valid    : out STD_LOGIC;
            data_out_last     : out STD_LOGIC;
            data_out_ready    : in  STD_LOGIC;
            compression_ratio : out STD_LOGIC_VECTOR(15 downto 0)
        );
    end component;
    
    component quic_packet_processor is
        Port (
            clk               : in  STD_LOGIC;
            rst               : in  STD_LOGIC;
            start             : in  STD_LOGIC;
            mode              : in  STD_LOGIC_VECTOR(1 downto 0);
            operation         : in  STD_LOGIC_VECTOR(1 downto 0);
            done              : out STD_LOGIC;
            connection_id     : in  STD_LOGIC_VECTOR(63 downto 0);
            packet_number     : in  STD_LOGIC_VECTOR(31 downto 0);
            data_in           : in  STD_LOGIC_VECTOR(63 downto 0);
            data_in_valid     : in  STD_LOGIC;
            data_in_last      : in  STD_LOGIC;
            data_in_ready     : out STD_LOGIC;
            data_out          : out STD_LOGIC_VECTOR(63 downto 0);
            data_out_valid    : out STD_LOGIC;
            data_out_last     : out STD_LOGIC;
            data_out_ready    : in  STD_LOGIC;
            ack_packet_num    : in  STD_LOGIC_VECTOR(31 downto 0);
            ack_receive       : in  STD_LOGIC;
            ack_send          : out STD_LOGIC;
            ack_ranges        : in  STD_LOGIC_VECTOR(127 downto 0);
            retx_needed       : out STD_LOGIC;
            retx_packet_num   : out STD_LOGIC_VECTOR(31 downto 0);
            retx_timeout      : in  STD_LOGIC_VECTOR(15 downto 0);
            packets_sent      : out STD_LOGIC_VECTOR(31 downto 0);
            packets_received  : out STD_LOGIC_VECTOR(31 downto 0);
            packets_retx      : out STD_LOGIC_VECTOR(31 downto 0)
        );
    end component;
    
    component axi_stream_interface is
        Generic (
            DATA_WIDTH      : integer := 64;
            USER_WIDTH      : integer := 8;
            BUFFER_DEPTH    : integer := 1024
        );
        Port (
            axi_aclk        : in  STD_LOGIC;
            axi_aresetn     : in  STD_LOGIC;
            s_axis_tdata    : in  STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
            s_axis_tkeep    : in  STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
            s_axis_tuser    : in  STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
            s_axis_tvalid   : in  STD_LOGIC;
            s_axis_tlast    : in  STD_LOGIC;
            s_axis_tready   : out STD_LOGIC;
            m_axis_tdata    : out STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
            m_axis_tkeep    : out STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
            m_axis_tuser    : out STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
            m_axis_tvalid   : out STD_LOGIC;
            m_axis_tlast    : out STD_LOGIC;
            m_axis_tready   : in  STD_LOGIC;
            quic_data_in    : in  STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
            quic_valid_in   : in  STD_LOGIC;
            quic_last_in    : in  STD_LOGIC;
            quic_ready_in   : out STD_LOGIC;
            quic_data_out   : out STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
            quic_valid_out  : out STD_LOGIC;
            quic_last_out   : out STD_LOGIC;
            quic_ready_out  : in  STD_LOGIC;
            module_select   : in  STD_LOGIC_VECTOR(3 downto 0);
            bypass_enable   : in  STD_LOGIC;
            status          : out STD_LOGIC_VECTOR(31 downto 0)
        );
    end component;
    
begin
    -- Clock and reset handling
    clk <= sys_clk;
    rst_n <= sys_rst_n;
    rst <= not rst_n;
    
    -- Control signal derivation
    module_select <= ctrl_reg_0(3 downto 0);
    operation_mode <= ctrl_reg_0(7 downto 4);
    bypass_enable <= ctrl_reg_0(8);
    start_process <= ctrl_reg_0(9);
    
    -- AXI Stream interface instantiation
    axi_interface: axi_stream_interface
    generic map (
        DATA_WIDTH   => 64,
        USER_WIDTH   => 8,
        BUFFER_DEPTH => 1024
    )
    port map (
        axi_aclk       => clk,
        axi_aresetn    => rst_n,
        
        s_axis_tdata   => s_axis_tdata,
        s_axis_tkeep   => s_axis_tkeep,
        s_axis_tuser   => s_axis_tuser,
        s_axis_tvalid  => s_axis_tvalid,
        s_axis_tlast   => s_axis_tlast,
        s_axis_tready  => s_axis_tready,
        
        m_axis_tdata   => m_axis_tdata,
        m_axis_tkeep   => m_axis_tkeep,
        m_axis_tuser   => m_axis_tuser,
        m_axis_tvalid  => m_axis_tvalid,
        m_axis_tlast   => m_axis_tlast,
        m_axis_tready  => m_axis_tready,
        
        quic_data_in   => quic_data_in,
        quic_valid_in  => quic_valid_in,
        quic_last_in   => quic_last_in,
        quic_ready_in  => quic_ready_in,
        
        quic_data_out  => quic_data_out,
        quic_valid_out => quic_valid_out,
        quic_last_out  => quic_last_out,
        quic_ready_out => quic_ready_out,
        
        module_select  => module_select,
        bypass_enable  => bypass_enable,
        status         => quic_status
    );
    
    -- QUIC Crypto module instantiation (need to adapt interface for 64-bit data path)
    crypto_inst: quic_crypto
    port map (
        clk            => clk,
        rst            => rst,
        start          => start_process and (module_select = "0001"),
        encrypt        => ctrl_reg_0(12),  -- 1 for encrypt, 0 for decrypt
        done           => crypto_done,
        
        -- Data interfaces need adaptation for 64-bit AXI-Stream
        data_in(63 downto 0)   => crypto_data_in,
        data_in(127 downto 64) => (others => '0'),  -- Only using lower 64 bits
        data_in_valid  => crypto_valid_in,
        data_in_ready  => crypto_ready_in,
        
        data_out(63 downto 0)  => crypto_data_out,
        -- Would need extra logic to buffer the full 128-bit output
        data_out_valid => crypto_valid_out,
        data_out_ready => crypto_ready_out,
        
        -- Parameters from control registers
        key            => ctrl_reg_3 & ctrl_reg_2,
        nonce          => ctrl_reg_1(31 downto 0) & ctrl_reg_1 & x"0000",
        aad            => ctrl_reg_3 & ctrl_reg_2,
        aad_valid      => '1',
        aad_ready      => open,
        auth_tag_in    => ctrl_reg_3 & ctrl_reg_2,
        auth_tag_out   => open,
        auth_result    => open
    );
    
    -- QUIC Compression module instantiation
    comp_inst: quic_compression
    port map (
        clk            => clk,
        rst            => rst,
        start          => start_process and (module_select = "0010"),
        compress       => ctrl_reg_0(13),  -- 1 for compress, 0 for decompress
        done           => comp_done,
        
        data_in        => comp_data_in,
        data_in_valid  => comp_valid_in,
        data_in_last   => comp_last_in,
        data_in_ready  => comp_ready_in,
        
        data_out       => comp_data_out,
        data_out_valid => comp_valid_out,
        data_out_last  => comp_last_out,
        data_out_ready => comp_ready_out,
        
        compression_ratio => open
    );
    
    -- QUIC Packet Processor module instantiation
    pkt_inst: quic_packet_processor
    port map (
        clk            => clk,
        rst            => rst,
        start          => start_process and (module_select = "0100"),
        mode           => ctrl_reg_0(15 downto 14),  -- Packet mode
        operation      => ctrl_reg_0(17 downto 16),  -- Operation
        done           => pkt_done,
        
        connection_id  => ctrl_reg_1,
        packet_number  => ctrl_reg_2(31 downto 0),
        
        data_in        => pkt_data_in,
        data_in_valid  => pkt_valid_in,
        data_in_last   => pkt_last_in,
        data_in_ready  => pkt_ready_in,
        
        data_out       => pkt_data_out,
        data_out_valid => pkt_valid_out,
        data_out_last  => pkt_last_out,
        data_out_ready => pkt_ready_out,
        
        ack_packet_num => ctrl_reg_3(31 downto 0),
        ack_receive    => ctrl_reg_0(18),
        ack_send       => open,
        ack_ranges     => ctrl_reg_3 & ctrl_reg_2 & ctrl_reg_1 & ctrl_reg_0,
        
        retx_needed    => open,
        retx_packet_num => open,
        retx_timeout   => ctrl_reg_0(31 downto 16),
        
        packets_sent   => open,
        packets_received => open,
        packets_retx   => open
    );
    
    -- Module interconnect based on module_select
    process(module_select, quic_data_out, quic_valid_out, quic_last_out, quic_ready_in,
            crypto_data_out, crypto_valid_out, crypto_last_out, crypto_ready_in,
            comp_data_out, comp_valid_out, comp_last_out, comp_ready_in,
            pkt_data_out, pkt_valid_out, pkt_last_out, pkt_ready_in)
    begin
        -- Default connections
        crypto_data_in <= (others => '0');
        crypto_valid_in <= '0';
        crypto_last_in <= '0';
        crypto_ready_out <= '0';
        
        comp_data_in <= (others => '0');
        comp_valid_in <= '0';
        comp_last_in <= '0';
        comp_ready_out <= '0';
        
        pkt_data_in <= (others => '0');
        pkt_valid_in <= '0';
        pkt_last_in <= '0';
        pkt_ready_out <= '0';
        
        quic_data_in <= (others => '0');
        quic_valid_in <= '0';
        quic_last_in <= '0';
        quic_ready_out <= '0';
        
        -- Route data based on module selection
        case module_select is
            when "0001" =>  -- Crypto module
                crypto_data_in <= quic_data_out;
                crypto_valid_in <= quic_valid_out;
                crypto_last_in <= quic_last_out;
                quic_ready_out <= crypto_ready_in;
                
                quic_data_in <= crypto_data_out;
                quic_valid_in <= crypto_valid_out;
                quic_last_in <= crypto_last_out;
                crypto_ready_out <= quic_ready_in;
                
            when "0010" =>  -- Compression module
                comp_data_in <= quic_data_out;
                comp_valid_in <= quic_valid_out;
                comp_last_in <= quic_last_out;
                quic_ready_out <= comp_ready_in;
                
                quic_data_in <= comp_data_out;
                quic_valid_in <= comp_valid_out;
                quic_last_in <= comp_last_out;
                comp_ready_out <= quic_ready_in;
                
            when "0100" =>  -- Packet Processor module
                pkt_data_in <= quic_data_out;
                pkt_valid_in <= quic_valid_out;
                pkt_last_in <= quic_last_out;
                quic_ready_out <= pkt_ready_in;
                
                quic_data_in <= pkt_data_out;
                quic_valid_in <= pkt_valid_out;
                quic_last_in <= pkt_last_out;
                pkt_ready_out <= quic_ready_in;
                
            when others =>  -- Bypass or unsupported configuration
                quic_data_in <= quic_data_out;
                quic_valid_in <= quic_valid_out;
                quic_last_in <= quic_last_out;
                quic_ready_out <= quic_ready_in;
        end case;
    end process;
    
    -- Register interface handling
    process(clk, rst_n)
    begin
        if rst_n = '0' then
            ctrl_reg_0 <= (others => '0');
            ctrl_reg_1 <= (others => '0');
            ctrl_reg_2 <= (others => '0');
            ctrl_reg_3 <= (others => '0');
            reg_rd_data <= (others => '0');
            reg_rd_valid <= '0';
            irq <= '0';
        elsif rising_edge(clk) then
            -- Clear one-shot signals
            reg_rd_valid <= '0';
            
            -- Register write access
            if reg_wr_en = '1' then
                case reg_addr(3 downto 0) is
                    when x"0" => ctrl_reg_0 <= reg_wr_data;
                    when x"1" => ctrl_reg_1 <= reg_wr_data;
                    when x"2" => ctrl_reg_2 <= reg_wr_data;
                    when x"3" => ctrl_reg_3 <= reg_wr_data;
                    when others => null;
                end case;
            end if;
            
            -- Auto-clear certain control bits
            if crypto_done = '1' or comp_done = '1' or pkt_done = '1' then
                ctrl_reg_0(9) <= '0';  -- Clear start bit
                irq <= '1';  -- Assert interrupt
            else
                irq <= '0';
            end if;
            
            -- Register read access
            if reg_rd_en = '1' then
                reg_rd_valid <= '1';
                case reg_addr(3 downto 0) is
                    when x"0" => reg_rd_data <= ctrl_reg_0;
                    when x"1" => reg_rd_data <= ctrl_reg_1;
                    when x"2" => reg_rd_data <= ctrl_reg_2;
                    when x"3" => reg_rd_data <= ctrl_reg_3;
                    when x"4" => reg_rd_data <= quic_status;  -- Interface status
                    when x"5" => reg_rd_data <= x"00000001" when crypto_done = '1' else x"00000000";  -- Crypto status
                    when x"6" => reg_rd_data <= x"00000001" when comp_done = '1' else x"00000000";   -- Compression status
                    when x"7" => reg_rd_data <= x"00000001" when pkt_done = '1' else x"00000000";    -- Packet processor status
                    when others => reg_rd_data <= (others => '0');
                end case;
            end if;
        end if;
    end process;
    
    -- Status LED indicators
    status_leds(0) <= not rst_n;                -- Reset indicator
    status_leds(1) <= quic_status(0);           -- Input FIFO empty
    status_leds(2) <= quic_status(2);           -- Output FIFO empty
    status_leds(3) <= crypto_done or comp_done or pkt_done;  -- Processing done
    
end Behavioral;
