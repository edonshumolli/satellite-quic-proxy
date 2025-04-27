----------------------------------------------------------------------------------
-- AXI Stream Interface Module
-- 
-- This module implements an AXI4-Stream interface for communication between
-- QUIC acceleration modules and the host system.
--
-- Features:
-- - Standard AXI4-Stream protocol implementation
-- - Configurable data width
-- - Support for TKEEP, TLAST, and TUSER signals
-- - Buffering and flow control
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity axi_stream_interface is
    Generic (
        DATA_WIDTH      : integer := 64;    -- Width of the data bus
        USER_WIDTH      : integer := 8;     -- Width of the TUSER signal
        BUFFER_DEPTH    : integer := 1024   -- Depth of FIFO buffers
    );
    Port (
        -- Clock and reset
        axi_aclk        : in  STD_LOGIC;
        axi_aresetn     : in  STD_LOGIC;
        
        -- AXI Stream input interface
        s_axis_tdata    : in  STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
        s_axis_tkeep    : in  STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
        s_axis_tuser    : in  STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
        s_axis_tvalid   : in  STD_LOGIC;
        s_axis_tlast    : in  STD_LOGIC;
        s_axis_tready   : out STD_LOGIC;
        
        -- AXI Stream output interface
        m_axis_tdata    : out STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
        m_axis_tkeep    : out STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
        m_axis_tuser    : out STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
        m_axis_tvalid   : out STD_LOGIC;
        m_axis_tlast    : out STD_LOGIC;
        m_axis_tready   : in  STD_LOGIC;
        
        -- Interface to QUIC processing modules
        -- Input from accelerator modules
        quic_data_in    : in  STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
        quic_valid_in   : in  STD_LOGIC;
        quic_last_in    : in  STD_LOGIC;
        quic_ready_in   : out STD_LOGIC;
        
        -- Output to accelerator modules
        quic_data_out   : out STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
        quic_valid_out  : out STD_LOGIC;
        quic_last_out   : out STD_LOGIC;
        quic_ready_out  : in  STD_LOGIC;
        
        -- Control and status signals
        module_select   : in  STD_LOGIC_VECTOR(3 downto 0);  -- Select which accelerator module to connect
        bypass_enable   : in  STD_LOGIC;                     -- Enable bypass mode (pass-through)
        status          : out STD_LOGIC_VECTOR(31 downto 0)  -- Status information
    );
end axi_stream_interface;

architecture Behavioral of axi_stream_interface is
    -- FIFO component for buffering
    component axi_stream_fifo is
        Generic (
            DATA_WIDTH   : integer := 64;
            USER_WIDTH   : integer := 8;
            BUFFER_DEPTH : integer := 1024
        );
        Port (
            clk          : in  STD_LOGIC;
            rst          : in  STD_LOGIC;
            
            -- Input interface
            s_tdata      : in  STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
            s_tkeep      : in  STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
            s_tuser      : in  STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
            s_tvalid     : in  STD_LOGIC;
            s_tlast      : in  STD_LOGIC;
            s_tready     : out STD_LOGIC;
            
            -- Output interface
            m_tdata      : out STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
            m_tkeep      : out STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
            m_tuser      : out STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
            m_tvalid     : out STD_LOGIC;
            m_tlast      : out STD_LOGIC;
            m_tready     : in  STD_LOGIC;
            
            -- Status
            fifo_count   : out STD_LOGIC_VECTOR(31 downto 0);
            fifo_full    : out STD_LOGIC;
            fifo_empty   : out STD_LOGIC
        );
    end component;
    
    -- Internal signals for input FIFO
    signal input_fifo_tdata   : STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
    signal input_fifo_tkeep   : STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
    signal input_fifo_tuser   : STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
    signal input_fifo_tvalid  : STD_LOGIC;
    signal input_fifo_tlast   : STD_LOGIC;
    signal input_fifo_tready  : STD_LOGIC;
    signal input_fifo_count   : STD_LOGIC_VECTOR(31 downto 0);
    signal input_fifo_full    : STD_LOGIC;
    signal input_fifo_empty   : STD_LOGIC;
    
    -- Internal signals for output FIFO
    signal output_fifo_tdata  : STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
    signal output_fifo_tkeep  : STD_LOGIC_VECTOR((DATA_WIDTH/8)-1 downto 0);
    signal output_fifo_tuser  : STD_LOGIC_VECTOR(USER_WIDTH-1 downto 0);
    signal output_fifo_tvalid : STD_LOGIC;
    signal output_fifo_tlast  : STD_LOGIC;
    signal output_fifo_tready : STD_LOGIC;
    signal output_fifo_count  : STD_LOGIC_VECTOR(31 downto 0);
    signal output_fifo_full   : STD_LOGIC;
    signal output_fifo_empty  : STD_LOGIC;
    
    -- Mux signals for multiple accelerator modules
    signal mux_data_in        : STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
    signal mux_valid_in       : STD_LOGIC;
    signal mux_last_in        : STD_LOGIC;
    signal mux_ready_in       : STD_LOGIC;
    
    signal mux_data_out       : STD_LOGIC_VECTOR(DATA_WIDTH-1 downto 0);
    signal mux_valid_out      : STD_LOGIC;
    signal mux_last_out       : STD_LOGIC;
    signal mux_ready_out      : STD_LOGIC;
    
    -- Status and control
    signal rst                : STD_LOGIC;
    signal packet_counter     : unsigned(31 downto 0);
    signal byte_counter       : unsigned(63 downto 0);
    
begin
    -- Reset signal generation
    rst <= not axi_aresetn;
    
    -- Input FIFO instantiation
    input_fifo: axi_stream_fifo
    generic map (
        DATA_WIDTH   => DATA_WIDTH,
        USER_WIDTH   => USER_WIDTH,
        BUFFER_DEPTH => BUFFER_DEPTH
    )
    port map (
        clk        => axi_aclk,
        rst        => rst,
        
        s_tdata    => s_axis_tdata,
        s_tkeep    => s_axis_tkeep,
        s_tuser    => s_axis_tuser,
        s_tvalid   => s_axis_tvalid,
        s_tlast    => s_axis_tlast,
        s_tready   => s_axis_tready,
        
        m_tdata    => input_fifo_tdata,
        m_tkeep    => input_fifo_tkeep,
        m_tuser    => input_fifo_tuser,
        m_tvalid   => input_fifo_tvalid,
        m_tlast    => input_fifo_tlast,
        m_tready   => input_fifo_tready,
        
        fifo_count => input_fifo_count,
        fifo_full  => input_fifo_full,
        fifo_empty => input_fifo_empty
    );
    
    -- Output FIFO instantiation
    output_fifo: axi_stream_fifo
    generic map (
        DATA_WIDTH   => DATA_WIDTH,
        USER_WIDTH   => USER_WIDTH,
        BUFFER_DEPTH => BUFFER_DEPTH
    )
    port map (
        clk        => axi_aclk,
        rst        => rst,
        
        s_tdata    => mux_data_in,
        s_tkeep    => (others => '1'),  -- Assuming all bytes are valid
        s_tuser    => (others => '0'),  -- Not using TUSER
        s_tvalid   => mux_valid_in,
        s_tlast    => mux_last_in,
        s_tready   => mux_ready_in,
        
        m_tdata    => m_axis_tdata,
        m_tkeep    => m_axis_tkeep,
        m_tuser    => m_axis_tuser,
        m_tvalid   => m_axis_tvalid,
        m_tlast    => m_axis_tlast,
        m_tready   => m_axis_tready,
        
        fifo_count => output_fifo_count,
        fifo_full  => output_fifo_full,
        fifo_empty => output_fifo_empty
    );
    
    -- Bypass logic (pass-through when enabled)
    process(bypass_enable, input_fifo_tdata, input_fifo_tvalid, input_fifo_tlast, 
            quic_data_in, quic_valid_in, quic_last_in, 
            mux_ready_out, quic_ready_out)
    begin
        if bypass_enable = '1' then
            -- Bypass mode - direct connection between input and output FIFOs
            mux_data_in <= input_fifo_tdata;
            mux_valid_in <= input_fifo_tvalid;
            mux_last_in <= input_fifo_tlast;
            input_fifo_tready <= mux_ready_in;
            
            -- Disable accelerator module connection
            quic_data_out <= (others => '0');
            quic_valid_out <= '0';
            quic_last_out <= '0';
            quic_ready_in <= '0';
        else
            -- Normal mode - route through accelerator modules
            -- Connect FIFO to QUIC modules
            quic_data_out <= input_fifo_tdata;
            quic_valid_out <= input_fifo_tvalid;
            quic_last_out <= input_fifo_tlast;
            input_fifo_tready <= quic_ready_out;
            
            -- Connect QUIC modules to output FIFO
            mux_data_in <= quic_data_in;
            mux_valid_in <= quic_valid_in;
            mux_last_in <= quic_last_in;
            quic_ready_in <= mux_ready_in;
        end if;
    end process;
    
    -- Module selection mux (would be more complex in full implementation)
    -- In a real system, this would route to different accelerator modules
    -- based on module_select signal
    
    -- Status counter for statistics
    process(axi_aclk)
    begin
        if rising_edge(axi_aclk) then
            if rst = '1' then
                packet_counter <= (others => '0');
                byte_counter <= (others => '0');
            else
                -- Count packets
                if m_axis_tvalid = '1' and m_axis_tready = '1' and m_axis_tlast = '1' then
                    packet_counter <= packet_counter + 1;
                end if;
                
                -- Count bytes
                if m_axis_tvalid = '1' and m_axis_tready = '1' then
                    -- Count valid bytes based on TKEEP
                    for i in 0 to (DATA_WIDTH/8)-1 loop
                        if m_axis_tkeep(i) = '1' then
                            byte_counter <= byte_counter + 1;
                        end if;
                    end loop;
                end if;
            end if;
        end if;
    end process;
    
    -- Assemble status information
    status(0) <= input_fifo_empty;
    status(1) <= input_fifo_full;
    status(2) <= output_fifo_empty;
    status(3) <= output_fifo_full;
    status(4) <= bypass_enable;
    status(15 downto 5) <= (others => '0');  -- Reserved
    status(19 downto 16) <= module_select;
    status(31 downto 20) <= (others => '0');  -- Reserved
    
end Behavioral;
