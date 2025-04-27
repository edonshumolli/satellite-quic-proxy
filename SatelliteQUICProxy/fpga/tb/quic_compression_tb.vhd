----------------------------------------------------------------------------------
-- Testbench for QUIC Compression Module
-- 
-- This testbench validates the functionality of the QUIC compression module
-- by simulating compression and decompression operations with test data.
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use STD.TEXTIO.ALL;
use IEEE.STD_LOGIC_TEXTIO.ALL;

entity quic_compression_tb is
-- Testbench has no ports
end quic_compression_tb;

architecture Behavioral of quic_compression_tb is
    -- Component Declaration for the Unit Under Test (UUT)
    component quic_compression
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
    
    -- Clock and reset signals
    signal clk           : STD_LOGIC := '0';
    signal rst           : STD_LOGIC := '1';
    
    -- Control signals
    signal start         : STD_LOGIC := '0';
    signal compress      : STD_LOGIC := '0';
    signal done          : STD_LOGIC;
    
    -- Data interface signals
    signal data_in       : STD_LOGIC_VECTOR(63 downto 0) := (others => '0');
    signal data_in_valid : STD_LOGIC := '0';
    signal data_in_last  : STD_LOGIC := '0';
    signal data_in_ready : STD_LOGIC;
    signal data_out      : STD_LOGIC_VECTOR(63 downto 0);
    signal data_out_valid: STD_LOGIC;
    signal data_out_last : STD_LOGIC;
    signal data_out_ready: STD_LOGIC := '0';
    
    -- Statistics
    signal compression_ratio : STD_LOGIC_VECTOR(15 downto 0);
    
    -- Clock period definition
    constant clk_period  : time := 10 ns;
    
    -- Test data constants and variables
    type data_array is array (natural range <>) of STD_LOGIC_VECTOR(63 downto 0);
    
    constant TEST_DATA_1 : data_array(0 to 9) := (
        x"0123456789ABCDEF", 
        x"0123456789ABCDEF", -- Repeated pattern to ensure compression
        x"FEDCBA9876543210",
        x"FEDCBA9876543210", -- Repeated pattern
        x"AAAAAAAAAAAAAAAAA",
        x"BBBBBBBBBBBBBBBB",
        x"CCCCCCCCCCCCCCCC",
        x"AAAAAAAAAAAAAAAAA", -- Repeated
        x"BBBBBBBBBBBBBBBB", -- Repeated
        x"CCCCCCCCCCCCCCCC"  -- Repeated
    );
    
    -- Variables to store compressed and decompressed data
    signal compressed_data : data_array(0 to 19) := (others => (others => '0'));
    signal compressed_count : integer := 0;
    signal decompressed_data : data_array(0 to 9) := (others => (others => '0'));
    signal decompressed_count : integer := 0;
    
    -- Procedure for sending data blocks
    procedure send_data_blocks (
        constant data_blocks : in data_array;
        constant block_count : in integer;
        signal data          : out STD_LOGIC_VECTOR(63 downto 0);
        signal valid         : out STD_LOGIC;
        signal last          : out STD_LOGIC;
        signal ready         : in  STD_LOGIC
    ) is
    begin
        for i in 0 to block_count-1 loop
            wait until rising_edge(clk);
            data <= data_blocks(i);
            valid <= '1';
            if i = block_count-1 then
                last <= '1';
            else
                last <= '0';
            end if;
            
            -- Wait until ready
            wait until ready = '1';
            wait until rising_edge(clk);
        end loop;
        
        -- Reset signals
        valid <= '0';
        last <= '0';
    end procedure;
    
    -- Procedure for receiving data blocks
    procedure receive_data_blocks (
        signal data_array    : out data_array;
        signal count         : out integer;
        signal ready         : out STD_LOGIC;
        signal valid         : in  STD_LOGIC;
        signal last          : in  STD_LOGIC;
        signal data          : in  STD_LOGIC_VECTOR(63 downto 0)
    ) is
        variable local_count : integer := 0;
    begin
        count <= 0;
        ready <= '1';
        
        while true loop
            wait until rising_edge(clk);
            if valid = '1' then
                data_array(local_count) <= data;
                local_count := local_count + 1;
                count <= local_count;
                
                if last = '1' then
                    exit;
                end if;
            end if;
        end loop;
        
        ready <= '0';
    end procedure;
    
begin
    -- Instantiate the Unit Under Test (UUT)
    uut: quic_compression
    port map (
        clk              => clk,
        rst              => rst,
        start            => start,
        compress         => compress,
        done             => done,
        data_in          => data_in,
        data_in_valid    => data_in_valid,
        data_in_last     => data_in_last,
        data_in_ready    => data_in_ready,
        data_out         => data_out,
        data_out_valid   => data_out_valid,
        data_out_last    => data_out_last,
        data_out_ready   => data_out_ready,
        compression_ratio => compression_ratio
    );
    
    -- Clock process definition
    clk_process: process
    begin
        clk <= '0';
        wait for clk_period/2;
        clk <= '1';
        wait for clk_period/2;
    end process;
    
    -- Stimulus process
    stim_proc: process
        variable match : boolean := true;
    begin
        -- Initialize inputs
        rst <= '1';
        start <= '0';
        compress <= '0';
        data_in <= (others => '0');
        data_in_valid <= '0';
        data_in_last <= '0';
        data_out_ready <= '0';
        
        -- Hold reset for 100 ns
        wait for 100 ns;
        rst <= '0';
        wait for clk_period*10;
        
        -- Compression test
        report "Starting compression test...";
        compress <= '1';  -- Set to compression mode
        
        -- Start compression
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send data to compress
        send_data_blocks(TEST_DATA_1, 10, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Receive compressed data
        receive_data_blocks(compressed_data, compressed_count, data_out_ready, data_out_valid, data_out_last, data_out);
        
        -- Wait for completion
        wait until done = '1';
        wait for clk_period*10;
        
        -- Report compression results
        report "Compression test completed";
        report "Original data size: " & integer'image(10 * 8) & " bytes";
        report "Compressed data size: " & integer'image(compressed_count * 8) & " bytes";
        report "Compression ratio: " & integer'image(to_integer(unsigned(compression_ratio))) & " (fixed point 8.8)";
        
        -- Decompression test
        report "Starting decompression test...";
        compress <= '0';  -- Set to decompression mode
        
        -- Reset for new operation
        wait for clk_period*10;
        
        -- Start decompression
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send compressed data to decompress
        send_data_blocks(compressed_data, compressed_count, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Receive decompressed data
        receive_data_blocks(decompressed_data, decompressed_count, data_out_ready, data_out_valid, data_out_last, data_out);
        
        -- Wait for completion
        wait until done = '1';
        wait for clk_period*10;
        
        -- Verify decompression results
        match := true;
        for i in 0 to 9 loop
            if i < decompressed_count then
                if decompressed_data(i) /= TEST_DATA_1(i) then
                    match := false;
                    report "Mismatch at index " & integer'image(i) & 
                           " - Expected: " & to_hstring(TEST_DATA_1(i)) & 
                           " Got: " & to_hstring(decompressed_data(i)) severity error;
                end if;
            else
                match := false;
                report "Missing data at index " & integer'image(i) severity error;
            end if;
        end loop;
        
        if match then
            report "Decompression test passed - All data blocks match original data!";
        else
            report "Decompression test failed - Data mismatch detected!" severity error;
        end if;
        
        wait;
    end process;

end Behavioral;
